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
| R-E | Phase 2 waits until the merged branch has no loose ends (the queue) |
| R1 | Variant signatures (the deck's σ) replace mainline's "determination" error |
| R2 | For the merge only: the solver path evaluates bound reducers and data checks in the tree; the direct path keeps SQL side tables. The lasting choice is OD-04 |
| R3 | A test that compares with stored output of a deleted evaluator is deleted, never re-recorded |
| R4 | `EXPLAIN` prints the rows an ABS or L0 rewrite adds |
| R5 | A row whose `per` key is NULL is in no class and gets no constraint |
| R6 | A clause that has classes but produces no model row warns |
| R7 | A message or plan line that breaks a promise of the guide is fixed. A row error with no clause or caret stays parked |
| R8 | Debug dumps stay behind one setting read once per query (`decide_debug`) |
| R9 | Dump-format tests go, except the two that check the deck's pictures (p. 81's tree, p. 91's containers) |
| R10 | On the direct path, which of several errors is reported is not owed |

## Open decisions

**First** blocks steps 3 to 5 of the [README](README.md).

**OD-01 (first). Error order on the solver path.** When a query has several faults, must a fixed one be reported?
Today an eight-key rank reproduces the order of a deleted evaluator; the guide promises no order.
- *Recommend:* unspecified, as in SQL and DuckDB: raise the first error reached. Keep only the rule that a row a
  `when` drops, or a value nobody reads, raises nothing.
- *Blocks:* CU-10, CU-16, Phase 2 (the binder raises most of the pinned errors).

**OD-02 (first). Marker names.** Internal markers are substrings of an expression's name, where a column's or a
decision's name also lives. Close it by refusing identifiers shaped like `__name__`, by a marker field of its own on
the expression, or by a typed home per fact.
- *Recommend:* the field now (CU-01). Typed homes are Phase 2's work.
- *Blocks:* CU-01.

**OD-03 (first). How much a message owes.** (a) Rewriting a removed spelling into the new one from the user's own
text. (b) A NaN message naming the column, not only the clause. (c) An `=`-bound message naming the class as well as
the group. (d) Tests comparing whole messages.
- *Recommend:* (a) one fixed message per removed form; (b), (c) name the clause; (d) assert the error class and the
  quoted clause.
- *Blocks:* CU-11, CU-14, CU-16.

**OD-04 (first). Where a bound is evaluated, and how direct solve reads the tree** (R2's lasting choice; R-C gives
the direction).
- *Recommend:* a rule analyses the bound tree, directly or through a view derived from it. A rule that fires lowers
  to relational operators; a miss leaves the solver path's meaning unchanged. The per-class variant rules get one
  implementation.
- *Blocks:* CU-26, [design.md](design.md) section 5.

**OD-05. Order inside Phase 2.**
- *Recommend:* front to back: parser, binder, canonicalizer, rewrites. An adapter lowers the new bound tree to
  today's tagged expressions for the stages not yet converted; its boundary moves back one stage at a time and it is
  deleted at the end (strangler fig). The branch stays runnable throughout.

**OD-06. Shape of the clause list.** One AND expression with tagged wrappers today.
- *Recommend:* a `vector<DecideClause>` (body, `when`, `per` key, source id, shape), the objective being one more
  clause, as part of Phase 2. A wrapper class of its own is the fallback if Phase 2 is delayed.

**OD-07. Constructs the deck omits.** The deck gives a node rule for SUM, MAX and frames only.
- *Recommend:* design a rule for ABS, products, L0, `<>`, AVG, norms, MIN/MAX beside other terms and quadratic
  terms, and present each as a departure (R-D). Put MIN/MAX on one route first (CU-20).
- *Blocks:* Phase 3.

**OD-08. Step 5's contract with the model builder.** Helper variables allocated at run time (names in `EXPLAIN` and
`DIAGNOSE` change)? Helper inlining decided at the node or by a whole-model pass? Evaluated rows one per group
instead of one per input row?
- *Recommend:* per group, decided at the node, allocated at run time. Measure inlining against solver presolve
  before keeping a whole-model pass.
- *Blocks:* Phase 3.

**OD-09. Thread model.** Each thread routes rows as they arrive and the routes are merged (478 lines, two run-time
guards). One route over the buffered rows in `Finalize` costs about 0.1 s per 2 million rows.
- The owner's call: it changes "reads each row as it arrives" into "one scan of the buffer". *Blocks:* CU-27.

**OD-10. Refusals in the objective.** Is refusing `MINIMIZE abs(sum(x) - 2)` a real gap or a stale gate (the same
algebra solves as a constraint)? Should `MAXIMIZE sum(v) + 1` be refused? Does the data-aggregate-factor gate still
guard anything?
- *Recommend:* switch each gate off and test. A gate that guards nothing goes. *Blocks:* CU-05, CU-15.

**OD-11. A refusal that reads only the plan** is raised at execution today.
- *Recommend:* raise it at planning; `EXPLAIN` and `PREPARE` then raise, and an empty input does not.
  *Blocks:* CU-06.

**OD-12. Direct-solve scope.** Two rules applying to one query: internal error, first match, or decline? Keep the
group-bound shortcut (0 to 25% CPU measured, about 500 lines)? R1's breadth (1,348 plan lines)?
- *Recommend:* assert overlap in debug, take the first match in release. The rest is the owner's call.
  *Blocks:* CU-07, CU-28.

**OD-13. Four merge decisions marked "owner to confirm".** (1) Two tests in `test_one_pass_tree.py` read a node's
reads from the dump's `reads=` field. (2) A product or square over a nested reducer under a bound that reads the
rows is refused by name (`repro/refusals.sql`, last query). (3) Four decline shapes in
`test_decidb_direct_decline.cpp` are written as an IR edit. (4) `test_decidb_helper_inlining.cpp` reads product
groups in two cases where OPA had four.
