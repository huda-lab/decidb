# ARCH-2 — design of Phase 6: the model is written at the nodes

Read against `2b948c6b44`. Marks: [read] from the code, [measured] on this host (Apple M1 Pro, Gurobi 13.0.3,
100,000 rows, median of 3).

## 0. Verdict

- The owner's rule (2026-11-23): **the optimizer chooses the logical form of every lowering; execution repeats that
  form once per group and decides nothing.**
- Under that rule the phase is two changes, not the five deletions of design.md § 4:
  1. **D89 moves from run time to plan time.** Whether a SUM or AVG helper is a variable or is written into its
     readers is a rule over the plan, in the optimizer. `InlineReducerValueLinks` and `PlaceHelpers` go.
  2. **A node writes one record per group.** The emitter stops building arrays as long as the input, and stage 06
     stops regrouping them.
- **The plan-time helper allocation stays.** A helper declared at planning is the logical form the owner's rule
  asks for. design.md § 4 lists it under "Delete"; that line is withdrawn, and helper names in `EXPLAIN` and
  `DIAGNOSE` do not change.

## 1. Measurements

### 1.1 The solver's presolve does not replace inlining [measured]

`benchmark/decide/anr_scaling.py`, with `InlineReducerValueLinks` on and switched off by a temporary patch (not
committed). Own = optimizer + model construction.

| Workload | Columns on / off | Own ms on / off | Solve ms on / off | Peak MB on / off |
|---|---|---|---|---|
| range-w1 (window read by its anchor) | 100,000 / 200,000 | 46 / 56 | 66 / 131 | 187 / 315 |
| range-w5 | 100,000 / 200,000 | 67 / 97 | 91 / 152 | 240 / 360 |
| range-w50 | 100,000 / 200,000 | 307 / 437 | 396 / 606 | 1114 / 1102 |
| cyclic-repeats | 100,000 / 200,000 | 55 / 78 | 69 / 196 | 204 / 353 |
| MAX, every group one member (REAL) | 100,000 / 200,000 | 50 / 63 | 57 / 112 | 121 / 254 |
| MAX, every group one member (INT) | 100,000 / 200,010 | 55 / 100 | 52 / 209 | 131 / 258 |
| nested-1, nested-2 | equal within 10 | equal | equal | equal |
| nested-3 | 110,100 / 111,110 | **1288 / 191** | 261 / 226 | **814 / 336** |

- Writing a window into its one reader, and a one-member MAX group as its member, each make the solve 1.5 to 4
  times faster and halve the columns. Both rules stay.
- nested-3 is 3.7 times slower end to end *with* today's pass (1.3 M nonzeros against 0.41 M). § 1.3 has the
  cause.

### 1.3 A MIN or MAX over group values has a member per input row [measured]

`per g1: sum(max(sum(x) by (g1, g2, g3)) by (g1, g2)) by (g1) <= 50` over 40 rows, 8 inner sums in 4 MAX groups:
the model has 40 link rows where 8 are distinct, and `anr.members` is 120, 40 per helper. Each row of the input
lists the inner sum it belongs to as a member of its own. At 100,000 rows nested-2 states 100,000 link rows for
1,000 distinct members. Writing the inner sum in then copies its ten terms into every duplicate, which is
nested-3's 1.3 M nonzeros. The count rule keeps the helper only because the duplicates raise `k`.

A member is one distinct instance of the child. With that, a sum under a MIN or MAX pinned on one side is read
once by construction, and the nest's rows fall from one per input row to one per child group.

### 1.2 Where the clauses-times-rows cost is [read]

`DecideModelEmitter::Groups` allocates six vectors as long as the input per clause (`kept`, `null_rows`, `active`,
`group_of`, `class_of_row`, `row_group_ids`) and a coefficient column per term. `ClauseEmittedRows`
(`ilp_model_builder.cpp:413`) then regroups them into `EmittedRows`, one row per group. `row_group_ids` has 135 uses
in 14 files; `row_coefficients` 29 in 12.

## 2. The split

| Job | Layer | Today | After |
|---|---|---|---|
| Native or lowered | optimizer | `SolverConstructSupport` copied onto the plan | unchanged |
| The form of a lowering (helper, its definition, the sides it is pinned on) | optimizer | `AddValueHelper`, `helper_definitions`, `push` | unchanged |
| Variable or written into its readers (D89) | stage 06, from the data and every reader | optimizer, from the plan (§ 3) |
| One instance of the form per group | execution | arrays per input row, regrouped by stage 06 | a record per group, written by the node (§ 4) |
| Big-M constants, boxes, implied bounds | `FormulateModel` | unchanged: `DIAGNOSE` re-runs it on a new box without reading data |

Pattern: instruction selection apart from emission. The optimizer selects; the node emits; `FormulateModel`
legalizes what needs the box (`<>`, a hard MIN or MAX), as LLVM's LegalizeDAG does.

## 3. D89 as a plan rule

Today D89 counts nonzeros over the built model (`m·k` against `m + 1 + k`) and asks every reader whether it can
take an expression. Proposed, read off the plan alone:

| Reducer | Form | Condition |
|---|---|---|
| SUM, AVG | **members passed up** | every reader takes a linear expression, and either it is in a nest of sums (the deck's rule, pp. 115, 119; today's `flattened`) or each instance has one reading row by construction (a window that moves with its row and is read by one row; a sum that is a member of a MIN or MAX pinned on one side) |
| SUM, AVG | variable, one definition row per group | any other |
| MIN, MAX | variable, one link per group | always |
| MIN, MAX, a group of one member | that member, no variable | a fixed part of the form (deck p. 112), when every reader takes a linear expression |

- "Takes a linear expression" is `PlaceHelpers`' test: a linear row of a plain clause, the linear objective, or one
  other helper's members. A product, a square, a frame read, an ABS, a `<>`, a hard MIN or MAX hold the column.
  [read] Every test is "which variable stands in which kind of reader", and each list is on the plan: a clause's
  terms and its `admitted_lower`, the product groups, the frame reads, the Boolean-product and ABS definitions with
  `exact`, the `<>` and quadratic clauses, the objective's terms, the other helpers' members, and the bounds folded
  into a box (`user_singleton_row_bounds`). The implied bounds are not a test: the pass only runs after them.
- [read] The deck's two models rest on the structural half already. `flattened`
  (`linearization_reducer_value.cpp:459`) marks a SUM or AVG that holds a nested reducer, and a SUM or AVG nested in
  one, with no count; the one-member MAX group (`c2`) is the last row of the table. So p. 119 stays 6 columns and 3
  rows and p. 115 stays 8 and 7. `test_anr_nested.py` pins both.
- What rested on the count alone: a lone helper read by a few rows (guide example 5), a window read by two rows, a
  sum under a MIN or MAX. The first two keep their column; the third is written in by construction (§ 1.3).
- **What changes for a user.** A lone SUM helper with few members read by few rows (guide example 5: two shipments
  read by two rows) keeps its column, where the count wrote it in. `EXPLAIN` states the form at planning; the
  `EXPLAIN ANALYZE` line that replaces the plan's (`VALUE_HELPER_INLINED`) goes for a SUM or AVG.
- **The one data fact left in execution** is the last row: a count of members per group, tested where the group
  is written. It is the deck's own rule and § 1.1 measures it at 2 to 4 times. The alternative under the strict
  reading of the owner's rule is a variable for every group and that cost.

## 4. The node contract

A node writes what the model builder reads, per group, in one walk children first:

| Node | Writes | Replaces |
|---|---|---|
| G, members passed up | nothing of its own: its groups' member lists are appended to the reader's row or member list | the inliner's expansion (`linearization_reducer_value.cpp:300-520`) |
| G, variable | per group: the members (`ReducerValueMembers`, already per group), one per distinct instance of the child (§ 1.3) | a member per input row; no later pass edits the list |
| O, range | per window: the members | unchanged shape |
| F (ABS, Boolean product, L0, definition) | the rows its definition states are root E's it owns, written as any root's | [read] `AbsEnvelope` calls `Rows(root)` for each of an ABS's two rows and stores them on `AbsLinkSpec`; a Boolean product is a pair of variables with no rows; an L0 or definition row is a clause row with `defined_var`. No separate writer exists |
| root E, aggregate clause | per group: the decision terms (`EmittedRows`), the bound, the group's name and class set | `EvaluatedConstraint::row_group_ids`, `row_coefficients`, `rhs_values` at input length; `ClauseRows`, `ClauseEmittedRows` |
| root E, per-row clause | per kept row: the same record | a row per input row with excluded rows marked |
| root E, objective | per term: instance and coefficient | unchanged |

- Evaluation stays vectorized (R23): coefficients, keys and folds are programs over chunks. A node reads its
  container, which holds the rows its `when` kept, so a clause that keeps 2,000 of 2,000,000 rows touches 2,000.
- A record holds no Big-M constant and no box. `FormulateModel` stays a pure function of the records and a box.
- Constructs the deck omits each get a row in this table's sense, and each is a departure (§ 6):

| Construct | Node rule |
|---|---|
| AVG | a SUM whose members carry 1/n, n the group's member count |
| COUNT | a SUM of the indicator the optimizer made |
| ABS, Boolean product, L0, a norm's definition | its F node writes the definition's rows per instance; Big-M numbers at `FormulateModel`; a native construct is a record the backend takes whole |
| `<>` | the root writes the group's row marked as a `<>`; `LinearizeNotEqual` adds the binary and the Big-M rows |
| MIN or MAX beside other terms | a variable (value shape), the plan's choice |
| quadratic terms | a root passes its product groups up per group, as a SUM passes members |

## 5. R37: terms split from the finished nodes

**Built in this phase, prototype first** (owner, 2026-11-23). One reading means a term cannot disagree with its
node, and the four agreement errors go. It lands after the node records (MODEL-5), which fix what a term names. Its
first step prototypes the splitter's leaf for a child read: the splitter has 10 users and 9 never see one. If the
prototype leaves the shared splitter worse, the evidence goes to the owner, who rules; it is not closed on an
estimate. Sizing: `TREE-25-sizing.md`.

## 6. Departures from the deck (R-D: the owner presents each)

1. Formulation is split in three (optimizer selects, node emits, `FormulateModel` legalizes); the deck has the
   node do all of it. Reason: one reader of every helper is needed to choose its form, and `DIAGNOSE` re-solves on
   a new box.
2. A lone SUM is written into its readers by structure, not by nonzero count (§ 3). The deck's example models are
   unchanged (§ 3); MODEL-1's comparison confirms it by run.
3. Node rules for AVG, COUNT, ABS, products, L0, norms, `<>`, quadratic terms (§ 4).

## 7. How it lands safely

Old and new run side by side in a debug build: the new path builds its records, the old path builds the model, and
a check compares the two models row by row over the whole suite. The old path is deleted in the task that makes the
comparison empty. A difference the rule of § 3 intends is listed for the owner before the task commits.

## 8. Tasks

| Id | Task | Deletes | Needs |
|---|---|---|---|
| MODEL-1 | The optimizer sets each reducer helper's form (§ 3) on its definition; a debug comparison against `InlineReducerValueLinks` lists every query of the suite where they differ | nothing yet | — |
| MODEL-2 | A G lists one member per distinct instance of its child; a G whose members are passed up hands them to its reader at the node; the one-member MIN or MAX group is written at the node | `InlineReducerValueLinks`, `PlaceHelpers`, `HelperReads`, the second `VarIndexer::Build`, `DecideVarScope::INLINED` (22 uses), `VALUE_HELPER_INLINED` for sums | MODEL-1 |
| MODEL-3 | Every reader of an evaluated clause in stage 06 (`<>`, ABS, Big-M, implied bounds, absence, the builder) reads its emitted rows: one record per group or kept row, with its bound, name and class set | the readers' own use of `row_group_ids` (135 uses in 14 files) and `row_coefficients` | MODEL-2 |
| MODEL-4 | A root E of an aggregate clause writes that record at the node, an F's rows among them | the six input-length vectors of `Groups`, `ClauseRows`, `ClauseEmittedRows`' regrouping | MODEL-3 |
| MODEL-5 | A per-row root writes a record per kept row | `row_group_ids` and `row_coefficients` themselves; the row loops of `decide_value_extraction.cpp` | MODEL-4 |
| MODEL-6 | Terms are split from the finished nodes (§ 5), prototype first | `LowerFrames` and its overloads, `RecordFallbackColumns`, one of `Translate`/`TranslateTree`, four agreement errors | MODEL-5 |
| MODEL-7 | Phase gate and docs: `ab_bench.py`, `repro/clauses_times_rows.sql` no slower in evaluation time or peak memory, HiGHS mode, debug build; design.md § 4, stages 05, 06, 08, guide § 6.4, language D89 | the side-by-side check | MODEL-6 |

MODEL-3 to MODEL-5 are one carrier, the evaluated clause, moved in design.md § 3.4's three steps: readers through
the record, the writer writes it, the old fields go.

Focused checks per task: `./test/decide/run_tests.sh -k "anr or nested or inlin or frame"`, the model goldens, and
`unittest "[decidb]"`. MODEL-4 and MODEL-5 add `repro/clauses_times_rows.sql`.

## 9. Rulings (owner, 2026-11-23)

R42 to R46 in `decisions.md`: the owner's rule of § 0; D89 as the plan rule of § 3; the one-member MIN or MAX group
tested at the node; plan-time helper allocation stays; R37 built prototype first. The departures of § 6 are the
owner's to present (R-D).
