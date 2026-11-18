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
| R20 | Every DECIDE tag in an expression's name goes, replaced by the tree's typed nodes, records and fields in Phase 5; no interim tag field. Until then a helper decision carries its origin as a field, and DECIDE refuses a user name that contains a tag (2026-11-18) |

## Open decisions

An open decision lives with its task as **Decision needed**, with the options and a recommendation; the queue
(`context/docs/todo.md`) marks it `(decision)`.
