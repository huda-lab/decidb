# Cleanup tasks

The findings of the 2026-11-18 review, at `38699104bc`. Labels and how to file a task: [README](README.md). Line
counts are reviewers' estimates. `OD-nn` is a decision in [decisions.md](decisions.md).

## Part 1. Defects

### CU-01. Internal markers live in names a user can write
- **Problem.** A marker is a substring of an expression's name, and a column reference's name is the column's.
  `HasDecideTag` is a substring search; 55 marker constants, about 300 call sites [measured].
- **Evidence** [reproduced], `repro/marker_names.sql`: a decision named `"__l0auto_ind_q"` flips an answer (12/0
  becomes 0/12); a column named `"w__query_wide_value__"` breaks a constraint; a column named
  `"__definition_function_0__"` raises an INTERNAL error.
- **Change.** (a) `FormulateModel` finds L0 links by the typed fact the tree already holds (`l0_auto_m`), not by
  `StartsWith(name, "__l0auto_ind_")` and `"__abs_aux_"`. (b) Markers move off the name (OD-02).
- **Done when.** Each pair in the repro file gives one answer, as tests.

### CU-02. A quadratic objective over a data reducer raises an INTERNAL error
- **Evidence** [reproduced], `repro/refusals.sql`: `MINIMIZE SUM(POWER(x - avg(t.score) by (t.g), 2))` raises
  "linear expression split into a non-linear term". The same body as a constraint answers. Both merge parents fail.
- **Change.** The objective reads the reducer as the constraint does. **Done when.** It answers as the oracle does.

### CU-03. A refusal names an internal column
- **Evidence** [reproduced]: `MINIMIZE SUM(b * ABS(x - score))` says "requires a finite upper bound on variable
  '__abs_aux_0__'. Add a constraint like '__abs_aux_0__ <= <bound>'".
- **Change.** The message names what the user wrote, or the helper takes its bound from the operand's.

### CU-04. HiGHS refuses a concave product in the objective
- **Evidence** [reproduced]: `MAXIMIZE sum((x - score) * (score - x))` answers on Gurobi; under
  `DECIDB_FORCE_SOLVER=highs` it is refused as "multiplies two decision variables, which needs Gurobi".
- **Cause** [read]: the product group is not marked a square, so `IsNonconvexProductGroup` reads it as bilinear.
- **Change.** Recognise the square, or keep the refusal and say so in the guide (the owner's call).

### CU-05. The binder judges algebra by name, with different answers for one property
- **Evidence** [reproduced]: `MAXIMIZE sum(v) + 1` is accepted with no decision in it. `MINIMIZE abs(sum(x) - 2)`
  and `MINIMIZE power(sum(x) - 3, 2)` are refused though the same algebra solves as a constraint. "The objective
  reads no decision" has four messages; `MAXIMIZE v + 1` gets one about `'+'`.
- **Cause** [read]: about 1,340 of `decide_binder.cpp`'s 2,098 lines are validators that classify the parsed tree by
  name, in parallel with binding. The architecture doc gives the algebra to stage 04.
- **Change.** The binder refuses only what needs the parsed tree (scope placement, user casts, names). "Reads no
  decision", vocabulary, reducer-argument shape and degree become one check in `DecideShapeCheck`, the same code for
  a constraint side and the objective. OD-10 settles the two objective refusals.
- **Done when.** One message per property; the hole is refused or ruled allowed. About 400 to 600 lines go.

### CU-06. A refusal that reads only the plan is raised at execution
- **Evidence** [reproduced]: `x(INT) ... x <= -1` raises when run, raises over an empty input (the guide says an
  empty input gives an empty result), and `EXPLAIN` prints a plan. The check is a 150-line constructor of
  `DecideGlobalSinkState`.
- **Change.** Run it at physical planning (OD-11).

### CU-07. Two rules applying to one query is a user-facing INTERNAL error
- **Evidence** [read]: `direct_rule_driver.cpp` throws when more than one rule applies. Nine boundary probes found
  no overlap today; the exposure is 36 rule pairs.
- **Change.** OD-12.

## Part 2. Message machinery

### CU-10. Errors are collected and ranked so that a fixed one prints
- **Problem.** Evaluation catches every error, continues, and raises the least by an eight-key rank
  (`decide_error_ledger.hpp`: phase, step, clause, part, expression, group, position, row). Every tree node carries
  rank fields; `ErrorRanks`, `WhenRanks` and `KeyedMappingOrder` are case lists per node kind. A vector that fails
  is retried row by row and every failing row's error is stored.
- **Evidence** [reproduced], `repro/error_cost.sql`: one failing `when` over 200,000 rows takes 3.1 s and 264 MB;
  plain SQL fails in 0.02 s. `test_error_precedence.py` (600 lines) pins the order. The guide promises none.
- **Change** (after OD-01). Raise the first error reached. Keep deferral only: a row a `when` drops, or an entry no
  parent reads, raises nothing. Delete `DecideBlock`, the rank fields, the three rank functions and `Rebase`.
- **Done when.** The repro fails in time close to plain SQL's. About 400 to 500 lines go.

### CU-11. A rewriter for removed syntax lives in the parser
- **Problem** [read]: `decide_removed_spellings.cpp` (909 lines) rebuilds what the old grammar would have parsed, to
  print the new spelling from the user's text. It keeps a second token (`WHEN_DECIDE_OBJECTIVE`) and lexer state,
  about ten error-only grammar productions, a walk over every `SelectNode` of every query, and 767 test lines. The
  spellings were removed on 2026-10-19.
- **Change** (after OD-03). One fixed message per removed form, raised at its production. About 800 lines go.

### CU-12. Clause text is stored five times and rendered eight times per comparison
- **Problem** [read]: `ConstraintSourceInfo` holds written, source, rendered, canonical and spelled text of each
  side. `InitializeConstraintSourceInfo` and `FinalizeConstraintSourceInfo` render on every query. Whether the
  canonical form differs is found by comparing two renders. About 45 display strings ride the four model types.
- **Change.** Keep the span id and one canonical rendering; cut written text from `DecideSourceText` on demand; the
  canonicalizer reports "changed". The diagnosis engines read these fields (26 uses), so change them together.

### CU-13. An ordinary query builds diagnosis state
- **Problem** [read]: every model row carries a `ConstraintProvenance` of about 20 fields, stamped with no
  `diagnose` gate. `plan_decide.cpp` spends about 190 lines harvesting column names whose only reader is the
  unbounded diagnosis. `ParseLiteralCoefficient` recovers a number from a display label with `strtod`.
- **Change.** Gate the stamping and the harvest on `diagnose`. Carry the unit coefficient as a number.

### CU-14. Machinery that only chooses what a message names
- **Problem** [read]: merged coefficients keep their original `inputs` so a NaN message names the first bad one
  (four layers); the direct path redoes it in SQL and adds dictionary codes (`DirectMessageCodes`, used by R1 only)
  so an aggregate can take the least message. An `=` bound runs a second, finer pass only to name the class.
  `DirectIR::model_shape` and `DirectStateErrors` classify a solver model nobody builds, to decide whether one hint
  sentence is appended. `check_position` keeps checks "where they would have stood".
- **Change** (after OD-03). Name the clause; one raising check per rule; one fixed sentence. About 500 lines go.

### CU-15. Two binder gates exist for wording or for a cause that is gone
- **Problem** [read]: the integer-step gate is implemented twice in `decide_binder.cpp`; the parsed half (about 125
  lines) exists for its wording, and the bound half predicts stage 04 from stage 02.
  `ValidateDecideNoDataAggregateFactor` cites a symbolic layer the canonicalizer replaced; `sum(x * avg(v)) <= 8`
  solves while `x * avg(v) <= 8` is refused [reproduced].
- **Change.** Delete the parsed half; run the bound half after canonicalization. Test the second gate off (OD-10).

### CU-16. Tests pin whole messages
- **Evidence** [measured]: 687 of 2,533 test functions touch message text; 17 functions (about 65 cases) assert
  error order; `test_direct_user_facing.py` is 1,699 lines.
- **Change** (after OD-01, OD-03). A test asserts the error class and the quoted clause. Order tests go with CU-10.

## Part 3. Structure that stands alone

### CU-20. MIN/MAX has five routes into one emitter
- **Problem** [read]: easy single term, hard single term, composed constraint, composed or flat objective, and the
  value route all end in `EmitExtremumLink`. The canonicalizer picks by shape.
- **Evidence** [reproduced], `repro/minmax_two_models.sql`: `sum(a*x) + max(b*x) <= 40` builds 13 rows; the same
  clause with a subquery bound builds 25. "composed" is on 364 lines in 17 files [measured].
- **Change.** Every decision reducer that is not an in-place SUM or AVG takes the value route. First scope the
  rewritten clause once per instance (removes the duplicate rows), then delete the composed route (about 700 lines,
  two side vectors on `LogicalDecide`). Hard single term and flat objective follow. Needs a benchmark gate.

### CU-21. ABS is lowered half in stage 05 and half in stage 06
- **Problem** [read]: `RewriteAbs` emits two comparison rows as expressions; stage 06 finds them back by index
  (`BuildAbsTagMap`), recovers the operand from one of them, and clones them into the rest. ABS is the only
  construct whose indicator is allocated in stage 05, so a rewrite pass reads `use_native_constructs`. Native
  against lowered is decided in three places under two policies. `SolverConstructSupport::bilinear` is never read.
- **Change.** Stage 05 records the ABS (helper, operand, exact or not); stage 06 emits every row. Stage 05 picks the
  shape and stage 06 the encoding, in one lowering pass. Delete `bilinear`. About 150 lines go.

### CU-22. Stage 05 is 19 ordered passes over one tree and six side containers
- **Evidence** [measured]: `RewriteBilinear` runs three times and `TagAbsForBigM` twice; the objective is
  re-canonicalized 9 times on a query no pass changed; 16 loops over side vectors; three polarity walkers
  (`WalkPolarity`, `CollectReducerSites`, `WalkComposedLhs`); two `ReadAffine`.
- **Change.** (a) One enumerator of every body, used by every pass. (b) Fold `CollectReducerSites` onto
  `WalkPolarity`. (c) Later: one post-order traversal for the node-local constructs, which ends the reruns.

### CU-23. One record carries five kinds of model row
- **Problem** [read]: `EvaluatedConstraint` has about ten construct-specific fields; a hard MAX arrives "spelled as
  an ordinary `SUM(e) >= K` row", so readers carry skip lists (`IsPlainLinearRow`). The provenance triple (source
  clause, removal group, repair group) is declared in 13 structs and copied field by field at three sites, one of
  which drops three fields.
- **Change.** Plain rows plus one typed list per construct; one provenance value type. Follows CU-20 and CU-21.

### CU-24. The operator file carries the model driver and the solve routing
- **Problem** [read]: `physical_decide.cpp` is 2,132 lines, about 425 of them the operator. `FormulateModel`, a
  second hand-ordered pass list, reads operator fields beside `SolverInput`. `FinalizeSolveResult` is 479 lines.
  `SolverModel::Build` is one function of 555 lines, complexity 119 [measured].
- **Change.** `SolverInput` becomes the whole contract and the emitter writes into it. `FormulateModel(input, box)`
  moves to `src/decidb/formulation` and stays a pure function, so the diagnosis can re-run it against a widened
  box. Solve-and-route moves beside the solver. Split `Build` into columns, objective, clause rows and raw rows.

### CU-25. A plan fact has three or four homes
- **Problem** [read]: `plan_decide.cpp` copies 35 fields by hand from `LogicalDecide` to `PhysicalDecide`; frames,
  signatures, keys and scopes are read from both the operator and the tree. Dead at execution: the tree's copy of
  the singleton-row boxes, `tree.decide_index`, `PhysicalDecide::scope_checks`.
- **Change.** The tree owns each fact; the operator keeps the child, the tree, the debug options and one display
  payload. The name harvest uses the tree's own enumeration of row expressions.

### CU-26. A data-valued bound has three evaluators
- **Problem** [read]: SQL side tables (direct path), a rule's own aggregate (`DecideGroupBound`), and the tree
  (solver path). `DecideOptimizer::RetainRowChecks` builds a whole direct front end to copy one vector; a check
  passes through three types; `DecideScopeCheckKind` has one value; solver-path files include direct headers.
  `direct_bound_tables.cpp` restates the per-class variant rules in SQL (about 105 lines).
- **Change.** Now: one vector of value checks, the enum and both wrapper types deleted (about 100 lines). The rest
  waits on OD-04.

### CU-27. Per-thread routing and its merge are heavy for what they buy
- **Evidence** [measured by a reviewer]: `DecideDescent::Merge` is 316 lines, complexity 69, with two run-time
  guards for shapes that cannot merge; it buys about 0.1 s of 1.3 s on 2 million rows.
- **Change.** OD-09.

### CU-28. Direct rules forward columns by hand
- **Problem** [read]: each projection stage of a non-ranked rule has a struct of bindings and forwards them by hand
  (R1: 17 fields at five stages). `direct_plan_fragments` is 1,711 lines over 11 topics.
  `DirectCanSkipSourceOutput` is a case list over operator types.
- **Change.** A stage builder that forwards everything and lets DuckDB's own pruning remove the rest (about 400
  lines), or only the shared row-box fragment (about 80). Leave `DirectRankedPrefix` alone. Scope: OD-12.

### CU-29. Small residue
- Two timing channels: `DECIDB_BENCH` beside `DecideProfileScope`. Keep the profile scope.
- Dead `Id`, `Ids`, `Mask` in `physical_decide.cpp`; a constant `true` member test compiled and run per entry.
- `decide_linear_form.hpp` still says `decide.prepared` and "last optimizer pass"; it runs at physical planning.
- Stage 03 and 04 are orchestrated inline in `Binder::CreatePlan(BoundSelectNode &)`, about 240 lines. Move them to
  one `PlanDecide` function.
- `architecture.md` lists seven edits to core DuckDB; ten exist (CTE column pruning in `remove_unused_columns.cpp`,
  `LogicalExtensionOperator::PruneUnusedOutputs`, the late-materialization call in `optimizer.cpp`) [read].
- Docs cite three tests that do not exist (`08_execution/done.md`, `o1_monge_assignment/done.md`,
  `testing/coverage.md`), and the direct-solve `done.md` files stamp 6,430 to 7,288 passed as "the baseline".

## Part 4. Leave for the design changes

Phases 2 and 3 replace this code, so cleaning it first is work done twice.

| What the review found | Replaced by |
|---|---|
| Each clause is lowered twice at planning (`DecideTreeConstruction` and `LowerDecideTree`), joined by expression address; 82 internal cross-checks | Phase 2: the rewrites produce the tree |
| One AND expression for all clauses, with `when` and `per` as tagged conjunctions | Phase 2: the clause record (OD-06) |
| `norm` bound as a `SUM` with a patched return type; a frame as a tagged aggregate guarded at 28 sites | Phase 2: node classes |
| `DecideNode` is one struct of 67 fields; `DecideTree::Validate` has complexity 142 | Phase 2: nodes split by kind |
| The emitter scatters node values into arrays as long as the input; evaluation scales with clauses times rows (`repro/clauses_times_rows.sql`) | Phase 3 |
| Helpers are created at planning and removed by `InlineReducerValueLinks` (300 lines, complexity 90) | Phase 3 |
