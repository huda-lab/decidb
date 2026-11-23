# TREE-8 — direct rules read `BoundDecide`: design for approval

Task: `context/docs/pipeline/todo.md` § TREE-8. Ruling R17: direct-solve rules read the tree directly; `DirectIR`
goes. Nothing here is coded yet.

## What `DirectIR` holds today

`DirectIR::Read` (`direct_ir.cpp`, 554 lines, class `DirectFrontEnd`) runs once per query and fills one struct that
every rule receives (`CheckCondition(const DirectIR &ir, …)`, 99 function signatures across 13 files). Its content
is three different things:

| Kind | Members | Where the same fact lives after TREE-3 … TREE-6 |
|---|---|---|
| A. Copies of bound facts | `decisions`, `keyed_scopes`, `row_checks`, `decide_index`, `render`, `place`; per constraint `written`, `comparison`, `rhs`, `scope.when`, `per_class`, `classes`, `aggregate`, `singleton_row`, `bound`, `variable_bound` | `BoundDecide` and `BoundDecideClause` fields, typed. The copy existed because these were alias tags only the front end could read. |
| B. A derived reading | per constraint `lhs`, the objective's `parts` and `offset`: each side split into `sign * scale * reducer(terms)` (`DirectPart`), with NULL-guarded coefficients and a `NOT_PROVED` status | Nowhere. It is an analysis of a clause body, made with the solver path's term splitter. |
| C. Facts of the source plan | `source_bindings`, `cross_product`, `not_null`, `keys` | Nowhere. They describe DECIDE's child operator, not the clauses. |

## Design

Pattern: the rules become ordinary optimizer rules over the bound operator (as DuckDB's rules read
`LogicalOperator`s), with shared reader functions; the Adapter that stood between them and the operator goes.

1. **Kind A goes.** A rule receives `const LogicalDecide &` and reads `decide.bound.clauses[i]`, the decisions and
   the keys directly. `DirectIRDecision`, `DirectIRKeyedScope`, `DirectIRScope`, `DirectIRConstraint`,
   `DirectIRObjective`, `DirectIR`, `DirectFrontEnd` and `DirectIRIsReadable` are deleted.
2. **Kind B becomes a shared reader, called on demand.** `DirectReadParts(decide, clause, parts)` returns false
   where the front end set `NOT_PROVED`, and the rule declines. `DirectPart` stays as its return type: it is the one
   view worth keeping. `PlainSum` and `Membership` become readers over a clause and its parts.
3. **Kind C stays as one small record**, `DirectSource` (the four members above), read once by the rule driver and
   passed beside the operator. `BoundDecide` has no place for facts of its child plan.
4. **The existing `DirectRead*` readers** (`direct_bound_reader`, 11 functions) take the operator and a clause
   record where they take `ir` and a `DirectIRConstraint`.
5. **`NULL_GUARD_TAG`** has two writers (`direct_ir.cpp:266`, `decide_linear_form.cpp:537`) and two readers
   (`DecideUnguarded`, the source render). Proposed home: the term holds its coefficient and, beside it, the
   condition that guards it; the `IF(valid, coefficient, 0)` is built where the value is evaluated, with no alias.
   `DecideUnguarded` goes because the unguarded coefficient is the field. To be confirmed by reading the six call
   sites before coding; if a caller only has the expression and not the term, the fallback is a node like the
   NULL-test node.

Rule signature after the change:

```cpp
virtual unique_ptr<DirectApplicabilityCondition>
CheckCondition(const LogicalDecide &decide, const DirectSource &source, ClientContext &context) const = 0;
```

## Decisions for the owner

| # | Question | Recommendation | Why | Cost of the other choice |
|---|---|---|---|---|
| 1 | Are the parts of a clause read on demand by each rule, or once by the driver and handed to every rule? | On demand | Matches R17 literally: no record stands between a rule and the tree. Rules decline on cheap tests first (decision count, clause count), so most never split a clause. | "Once by the driver" keeps a per-clause table of parts: a smaller `DirectIR` under another name. It saves re-splitting when several rules read the same clause; planning time only, a few clauses per query. |
| 2 | Does the source record (`DirectSource`) stay? | Yes | Its four facts come from walking DECIDE's child; nine rules would otherwise each repeat that walk. | Without it: the walk moves into readers called per rule. |
| 3 | The front end's tests (`test_decidb_direct_ir.cpp`, 24 cases) | Cases that check a copied record are deleted with the record; cases that check the split into parts move to `DirectReadParts`. | Code deleted, its tests deleted in the same change. | — |
| 4 | Whole-query gate: today a rule is offered the IR only if every clause and the objective are `PROVED`. | Each rule declines when a reader it calls returns false. | No gate object is left to hold the status. A rule that never reads a clause no longer declines because of it; rules are disjoint by shape, so this should change no outcome, and the direct matrix checks it. | Keep a driver pass that reads every clause's parts first: that is choice 1's other option. |

## Moves

| Move | What | Check |
|---|---|---|
| a | Rules and readers stop naming `ir.constraints[i].<copied field>`; they go through reader functions | focused direct tests |
| b | Readers take the operator and clause records; `DirectReadParts` on demand; `DirectSource`; the NULL guard's field | focused direct tests |
| c | The IR types, the front end, `NULL_GUARD_TAG`; tests that only reached them; docs (`direct_solve/architecture/ir.md` is rewritten as the readers' contract) | focused direct tests |

The direct matrix (`test/decide/run_direct_matrix.sh`) and the full suite run at the phase gate.

## TREE-8 — form

Written before any code (HEAD `d462b4ad31`, tree clean, no file of the repository changed). Three findings stop the
work before move (a); each needs the owner. The reader list below is the form the code takes once they are settled.

### Moves (a) and (b) are one change

A rule's signature changes from `(const DirectIR &, ClientContext &)` to `(const LogicalDecide &, const DirectSource &,
ClientContext &)`. An accessor layer over `DirectIR` for move (a) would be written and deleted in the next move, so
(a) and (b) land together; (c) deletes what is left.

### What each rule reads

Every rule that applies reads the parts of every clause and of the objective (checked rule by rule), so a clause the
readers cannot split declines every rule, as the whole-query gate does today.

| Rule | Operator fields | Derived readings | Source facts |
|---|---|---|---|
| A1 | decisions (domain, scope), `keyed_scopes` empty, clause shape | singleton row of each clause; bounds (`DirectReadBounds`); objective parts as quadratic parts | bindings |
| A2 | one REAL decision, its scope and keyed scope's key | singleton rows; bounds; one objective part (square, ABS, MAX of ABS) | bindings |
| R1 | REAL row decisions, `keyed_scopes` empty, clause shape | budgets: plain sum, shared scope, linear part, limit; bounds; objective linear parts | bindings |
| R4 | one REAL decision, row or keyed, its key | budget: plain sum, scope, linear part, limit; bounds and each bound's coefficient; one MIN part | bindings |
| N1 | one REAL row decision | one aggregate clause: scope, plain sum, square term, limit; bounds; objective quadratic parts | bindings |
| O1 | one BOOL row decision, two clauses, `per` and class key | each clause: membership, plain sum, `by` key against the class key; one linear objective part | bindings, `cross_product`, `not_null`, `keys` |
| S1 | one BOOL row decision | fixes and count bounds (`DirectSplitFixes`), shared scope, count body, count limit, fixed values; linear score | bindings |
| S2 | one BOOL row decision | as S1, one scope per level | bindings |
| S3 | one BOOL row decision | fixes; one budget: scope, plain sum, linear part, limit; objective linear parts | bindings |

### Readers

`DirectSource` (kind C) is read once by the driver: `source_bindings`, `cross_product`, `not_null`, `keys`
(`DirectSource::Read(const LogicalDecide &)`; `DirectIRCrossProduct` becomes `DirectCrossProduct`).

Read off the operator, no reader: clause count, `clause.shape` (aggregate or per row), the comparison's type and right
side (`clause.body`), `clause.scope.when`, `clause.scope.HasPer()`, `decide.bound.keyed_scopes`,
`decide.bound.variable_scopes[i]`, `decide.value_checks` (the driver's row checks), `decide.decide_index`.

Derived, each called by the rule that wants it (all return false where the front end set `NOT_PROVED`):

```cpp
DirectDomain DirectDomainOf(const LogicalDecide &decide, idx_t decision);
DecideClauseRef DirectClauseRef(const LogicalDecide &decide, const BoundDecideClause &clause); // the objective too
bool DirectReadKey(const LogicalDecide &decide, const DirectSource &source, idx_t key, vector<DirectKeyElement> &out);
//! One clause's left side as parts. False for: no source clause, VALUE shape, `per` on a per-row clause, a key or a
//! term the plan cannot read, a frame, a reducer `per` with a signature.
bool DirectReadParts(const LogicalDecide &decide, const DirectSource &source, ClientContext &context,
                     const BoundDecideClause &clause, vector<DirectPart> &parts);
//! The objective's parts. False for: a nested reducer, a signature, an offset that is not finite, an unread part.
bool DirectReadObjectiveParts(const LogicalDecide &decide, const DirectSource &source, ClientContext &context,
                              vector<DirectPart> &parts);
bool DirectReadSingletonRow(const LogicalDecide &decide, ClientContext &context, const BoundDecideClause &clause,
                            DecideSingletonRow &bound);   // DecideReadSingletonRow on the clause's comparison
const DirectPart *DirectPlainSum(const BoundDecideClause &clause, const vector<DirectPart> &parts);
unique_ptr<Expression> DirectMembership(const BoundDecideClause &clause, const vector<DirectPart> &parts, bool &ok);
```

The eleven `DirectRead*` readers of `direct_bound_reader` and the three of `direct_plan_fragments`
(`DirectReadScope`, `DirectProveSharedScope`, `DirectLinearScore::Prove`) take `(decide, source, …)` and, where they
took a `DirectIRConstraint`, the clause and its parts. `DirectKeyElement`, `DirectPart`, `DirectReducer` and
`DirectDomain` stay: they are what the readers return. `DirectAnalysisStatus` goes (a reader returns false).

Correction to the design's table: `singleton_row`, `bound` and `variable_bound` are kind B, not kind A. No field of
`BoundDecideClause` holds them when the rules run (`folded_bound` is set later, on the solver path); they are
`DecideReadSingletonRow`'s reading, made on demand like the parts.

Planning cost: a clause is split once per query today. On demand it is split once per rule that gets past its tests
on the decisions (a rule reads a clause's parts once and hands them to the readers): at most four times for a query
with one BOOL row decision (S1, S2, S3, O1), at most five for REAL decisions (A1, A2, R1, R4, N1), and never for a
query whose decisions fit no rule.

### Finding 1 — NULL guard: the field form does not hold (stopped, as instructed)

Writers: `DropNullObjectiveTerms` (`decide_linear_form.cpp:522`) wraps a `DecideTerm`'s coefficient in
`COALESCE(c, 0)`; `GuardCoefficients` (`direct_ir.cpp:258`) wraps a `DecideSplitTerm`'s in `IF(valid, c, 0)`.

| Reader | Has the term? |
|---|---|
| `RenderSource` (`decide_source_provenance.cpp:383`) | No. It meets the guard inside a larger expression: `CollectLikeTerms` runs after `DropNullObjectiveTerms` and merges like terms into `COALESCE(a,0) + COALESCE(b,0)` (`DecideMergeLikeCoefficients`), so one term's coefficient holds two guards. One condition beside the coefficient cannot say that. |
| `ThrowInvalidValue` (`decide_value_extraction.cpp:167`) | No. It has `ValueSource::value`, the program's written expression, at execution. |
| `DecideNameValue` (`decide_value_checks.cpp:22`) | No. An expression and its text. |
| `decide_rewrite_abs.cpp:245` | Yes (a `DecideSplitTerm`); the coefficient there is never guarded, so the call reads nothing through. |
| `DirectScaledMagnitude`, `DirectFoldConstantSum` (`direct_bound_reader.cpp:267`, `:399`) | No. A `DirectSignedValue` (sign and expression) or a bare expression. |
| O1 `SkipCasts(DecideUnguarded(e))` (`o1_monge_assignment_rule.cpp:121`), N1 `DecideIsGuarded` (`n1…:187`), the IR's `place` (`direct_ir.cpp:70`) | No. Expressions taken from terms several records earlier. |

On the direct path the guarded coefficient is copied as a plain expression into `DirectSignedValue`,
`DirectLinearTerm`, `DirectConstantTerm` and `DirectWeightedForm`, and the plans evaluate it from there; a field on
the term would have to be carried through each of those records and applied at every place a plan evaluates one.

Recommendation: the fallback, a node class. `BoundDecideNullGuardExpression`, a Decorator like
`BoundDecideNullTestExpression`: it holds the guarded expression (the COALESCE or the CASE) as its child, executes as
it, and `DecideUnguarded`/`DecideIsGuarded` and the render test the class in place of the alias. Both writers build
it; nothing else changes shape. One point to settle with it: DuckDB's rewriter does not look inside a class it does
not know, so a direct plan either unwraps the node where `StripTagsAbove` runs today or keeps it and may fold less
(plans and `EXPLAIN` to be compared). Not chosen here.

### Finding 2 — the rules' decline tests edit the IR (not covered by the approved design)

`test/common/test_decidb_direct_decline.cpp` (1,939 lines, 22 cases, 18 of them an applies and a declines case per rule) installs a probe rule that
receives the driver's `DirectIR`, edits it, and offers it to the rule under test: about 76 edits reach arms that no
SQL reaches. With no IR there is nothing to edit between the tree and the rule.

- About 37 edits set a field that is a copy of a bound fact (`keyed_scopes` 8, a decision's scope 3, a clause's
  right side 21, a clause's `when` 5). They become the same edit on the operator.
- About 41 edit a derived part (`objective.parts[…]` 24, a clause's `lhs` part 16, `singleton_row` 1): a part's sign,
  qualifier, filter, scale, a term's coefficient, a square's form. The rule now computes these itself, so the edit has
  to be made in the bound expression so that the reader derives the same part, or, where no tree derives it, the arm
  is unreachable and goes with its test.
- The edit now lands on the real operator, which then plans on the solver path (the probe declines), so an edited
  query must stop there (the probe raises once it has recorded the answer).

Recommendation: edits move to the operator; each of the 41 is settled one at a time (a tree edit, or the arm and its
case deleted as unreachable), and the hand-back lists which. This is the larger half of TREE-8's test work and can
remove rule code, so the owner should approve it before it starts. The other choice, keeping a way to hand a rule
edited parts, is an adapter for the tests alone.

### Finding 3 — three whole-query facts need a home once the gate goes

The gate also declines for facts of no single clause: `decide.bound.frames` not empty, a keyed scope with a
signature, an objective offset that is not finite. Proposed: the offset in `DirectReadObjectiveParts`; the signature
at the reducer whose `per` names that scope (`DirectReadParts`); a frame where it stands, a reducer node the part
reader already declines. A frame over data alone (`x <= at(previous else 3: cap) over (id)`) stands in a right side
or a coefficient, which the value predicates (`DirectIsDecisionFreeDeterministic`) must then refuse as a node that
is no value of the source row. The alternative is two lines in the driver (`frames` not empty: no rule is offered),
which is a small whole-query gate. Recommendation: the readers, checked by the frame cases of the direct tests.
