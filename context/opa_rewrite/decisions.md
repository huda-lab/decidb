# Decisions

Rulings the owner has made, and decisions still open. A ruling binds later work. An open decision carries a
recommendation; nobody settles one silently. Rulings about a single task live with that task in `context/docs`.

## Rulings in force

| # | Ruling |
|---|---|
| R-A | Continue with `Sami-OPA`; `Sami-ANR` is dropped |
| R-B | The goal is the deck's one-pass algorithm, all five steps |
| R-C | The parser builds the DECIDE tree, and that tree is the one central representation: binding, canonicalization, rewrites, direct solve and evaluation all work from it |
| R-D | A departure from the deck needs a clearly better design, which the owner presents to his supervisor first |
| R-E | Phase 5 waits until the merged branch has no loose ends: Phases 0 to 4 of the queue |
| R1 | Variant signatures (the deck's σ) replace mainline's "determination" error |
| R2 | For the merge only: the solver path evaluates bound reducers and data checks in the tree; the direct path keeps SQL side tables. The lasting choice is R17 |
| R3 | A test that compares with stored output of a deleted evaluator is deleted, never re-recorded |
| R4 | `EXPLAIN` prints the rows an ABS or L0 rewrite adds |
| R5 | A row whose `per` key is NULL is in no class and gets no constraint |
| R6 | A clause that has classes but produces no model row warns |
| R7 | A message or plan line that breaks a promise of the guide is fixed. A row error with no clause or caret stays parked |
| R8 | Debug dumps stay behind one setting read once per query (`decide_debug`) |
| R9 | Dump-format tests go, except the two that check the deck's pictures (p. 81's tree, p. 91's containers) |
| R10 | On the direct path, which of several errors is reported is not owed |
| R11 | The solver path raises the first error reached; no order among errors is owed. A row a `when` drops, or a value no parent reads, raises nothing (2026-11-18) |
| R12 | A message states the kind of error and where it is in the query, and stops there: it does not tell the user how to fix it (2026-11-18) |
| R13 | A removed spelling is an ordinary syntax error; no code recognises old syntax (2026-11-18) |
| R14 | A NaN or infinite value raises a message that says so and names the column or value where it stands (2026-11-18) |
| R15 | An `=` bound that takes two values names its `by` group, not its class: the pass that names the class runs on every query (2026-11-18) |
| R16 | A test of an error asserts its kind and the location it quotes, not the whole message (2026-11-18) |
| R17 | Direct-solve rules read the tree directly; `DirectIR` goes. A rule that fires lowers to relational operators and takes its bound values from SQL side tables; the solver path evaluates them in the tree. It lands in Phase 5, once the tree exists before the rules run (2026-11-18) |
| R18 | `DecideGroupBound` goes: a direct rule reads a bound's values from side tables, as for every other bound. No benchmark: the change removes a representation (2026-11-18) |
| R19 | R12 covers every error message, not only the NaN and direct-solve ones; `DIAGNOSE` proposes edits (2026-11-18) |
| R20 | Every DECIDE tag in an expression's name goes, replaced by the tree's typed nodes, records and fields in Phase 5; no interim tag field. Until then a reader tells a helper from a user decision by the tree's typed function nodes (no origin field), and DECIDE refuses a user name that contains a tag or a helper-name prefix (2026-11-18) |
| R21 | Phase 5 goes one carrier at a time across every stage that reads it, with no adapter: parser nodes, clause records, the reducer node, the frame node, function nodes and helpers, one lowering walk, direct rules. The canonicalizer, the clause walkers and the term splitter are called from both sides of every stage boundary, so a stage-by-stage order would keep two of each (2026-11-22) |
| R22 | The clause list is a vector of clause records (scope, body, origin, and the clause's facts), in place of one AND expression with tagged wrappers (2026-11-22) |
| R23 | `DecideTree` stays the executor's structure, derived from the bound tree by one lowering walk at physical planning. Execution stays vectorized: no task of Phase 5 or 6 adds a loop over input rows (2026-11-22) |
| R24 | A scope (`when`, `per`) is a record inside a clause or a reducer node, not a node of its own (2026-11-22) |
| R25 | A thing several places read is a record in a table, and a node names it by a typed index: keys, frames, and one table of helper definitions that owns each helper's body (2026-11-22) |
| R26 | A written clause's shape and variant signature are stored once by the canonicalizer and never recomputed. Bound reduction records the fact it relied on, and a debug build checks that record after each pass (2026-11-22) |
| R27 | A departure from the design stays when the built form is the better architecture: cleaner and more elegant code, no bloat, simpler, fewer ways to go wrong. It is brought to the owner when it is made, and the design is edited only after he rules (2026-11-23) |
| R28 | A frame is its own expression class, `BoundDecideFrameExpression`, holding a frame index and its payload; the frame's reducer lives in its `DecideFrameSpec` alone (2026-11-23) |
| R29 | R26 reworded: a clause's shape has one writer, the canonicalizing boundary, which sets it each time it canonicalizes the clause; no reader recomputes it (2026-11-23) |
| R30 | Helper rows print in the order the helpers are made, one table read by one loop; a stored plan of a query mixing helper kinds pins it (2026-11-23) |
| R31 | A reducer body has two readings, what its node reads and its coefficient algebra, by two readers; a clause's atoms are handed from one to the other with their nodes, and nothing joins them by address. Whether terms are split from the finished nodes is ruled after it is sized (2026-11-23) |
| R32 | `when c: (a and b)` binds one constraint per comparison, each under the `when`. Phase 5 runs focused tests per task, `make decide-test` at the end of each queue group, and every other suite once at the end (2026-11-23) |
| R33 | An AVG has one encoding: the node is an AVG from the binder to lowering, the AVG pass only records its account, and the linear form scales its terms. `EXPLAIN` prints no rewritten `sum(...)` layer for an AVG clause. The frame class lands before the reducer's kind, so the kind has the six real members and no placeholder (2026-11-23) |
| R34 | A member's NULL test stays a marker node, `BoundDecideNullTestExpression`, inside the reducer's rows. The rows are copied into about ten other row predicates (term filters, an ABS helper's rows, a definition's `when`, a direct part), and the tree tells a test from a written condition in each; a field on the reducer would make each carrier a pair, and a carrier that dropped the tests would read NULL rows with no error (2026-11-23) |
| R35 | The objective nest `sum(per K: inner(e) by (G))` stays as written. The tree lowers it, and the linear form reads it as terms (`decide_rewrite::ReadNestInPlace`). No copy of the nest is kept and the reducer has no `retained` field (2026-11-23) |
| R36 | `written_name` stays a display field. The binder spells each function one way (`RespellDecideAliases`), so the name it was written under is the one fact left. The query text has no name for a node canonicalization builds, and leaving aliases unrenamed would make every name check test an alias set (2026-11-23) |
| R37 | Terms are split from the clause in Phase 5: a reducer body keeps its two readings (R31). Whether terms are split from the finished nodes is decided with the model contract in ARCH-2, which reads `review/TREE-25-sizing.md` (2026-11-23) |
| R38 | The parsed reducer's `call` stays a `ParsedExpression`: a scope may stand in any call, and the binder refuses one that is no reducer with the text as written (2026-11-23) |
| R39 | The NULL guard is a decorator class around the guarded value, as the member NULL test is (R34); `NULL_GUARD_TAG` goes (2026-11-23) |
| R40 | The direct rules' decline tests edit the operator. An arm no tree can reach is deleted with its case (2026-11-23) |
| R41 | A whole-query fact a direct rule declines on (a frame, a keyed scope with a signature, a non-finite objective offset) is read by the reader that meets it; the driver holds no gate (2026-11-23) |
| R42 | The optimizer chooses the logical form of every lowering and stamps it on the plan; execution repeats that form once per group and decides nothing; `FormulateModel` supplies what needs the box (2026-11-23) |
| R43 | Whether a SUM or AVG helper is a variable or is written into its readers is a rule over the plan, set by the optimizer on the helper's definition: written in when every reader takes a linear expression and it is in a nest of sums or each instance has one reading row by construction. The nonzero count of language D89 goes (2026-11-23) |
| R44 | A MIN or MAX group of one member is that member: a fixed part of the form, tested at the node from the group's member count, the one data fact execution tests (2026-11-23) |
| R45 | Helpers stay declared at planning. A node allocates no variable, and helper names in `EXPLAIN` and `DIAGNOSE` do not change (2026-11-23) |
| R46 | R37 settled: terms are split from the finished nodes in Phase 6, prototype first. If the prototype leaves the shared splitter worse, the evidence goes to the owner (2026-11-23) |

## Open decisions

An open decision lives with its task as **Decision needed**, with the options and a recommendation; the queue
(`context/docs/todo.md`) marks it `(decision)`.
