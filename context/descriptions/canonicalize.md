# DECIDE constraint canonicalization

**Status (2026-08-12):** the five duplicate constraint-shaping paths have been
deleted and Step 1's adversarial contract tests are landed, but canonicalization
is **not complete**. The remaining work is to make those regressions green: fix
two correctness defects, implement composed reducer scales, centralize
homogeneity validation, and enforce the invariant after optimizer rewrites.

This document is the authoritative specification and remaining-work list for
constraint canonicalization. It describes current code, not the history of the
refactor.

---

## 1. Purpose and ownership

> One planning boundary decides the structural shape of every DECIDE
> constraint. Downstream stages consume that shape and never re-decide it.

The boundary consists of two operations in the canonicalization module:

1. **Canonicalize** a bound constraint tree into the structural form described
   below.
2. **Validate** that the resulting comparison is a supported per-row or
   aggregate constraint.

Keeping transformation and user-facing validation as separate operations is
acceptable. The important invariant is that both happen at one boundary,
before optimization, and no later layer partitions the comparison again.

The implementation lives in:

- `src/planner/decide/decide_canonicalizer.cpp`
- `src/include/duckdb/planner/decide/decide_canonicalizer.hpp`

There are two canonicalization entry points and there must not be a third:

| Entry point | Covers |
|---|---|
| `Binder::CreatePlan`, after `PlanSubqueries` | Constraints written by the user |
| `LogicalDecide::AddConstraint` | Constraints synthesized by `DecideOptimizer` |

`AddConstraint` is the only supported way to append a constraint to an existing
`LogicalDecide` operator.

---

## 2. Place in the pipeline

```text
parse and desugar
        |
        v
bind names, types, scopes, reducers and subqueries
        |
        v
PlanSubqueries and retain query-wide/correlated provenance
        |
        v
canonicalize + validate constraints        <-- single shape boundary
        |
        v
LogicalDecide
        |
        v
optimizer rewrites and generated constraints
        |
        v
VerifyCanonical
        |
        v
physical extraction and runtime RHS/group evaluation
        |
        v
solver-neutral model -> backend
```

This boundary affects the rest of the pipeline as follows:

| Stage | Responsibility after canonicalization is complete |
|---|---|
| Parser / desugaring | Parse DECIDE syntax, repair grammar ownership, and desugar constructs such as `norm()` and `IN`. It does not move comparison terms. |
| Binder | Resolve names, types, variable scopes, reducers and subquery correlation. It determines whether a comparison is a DECIDE constraint, but does not flip or repartition it. |
| Canonicalizer | Put every accepted bound comparison into the structural contract in §3 and reject unsupported mixtures once. |
| Logical plan | Store canonical constraints. New constraints enter only through `AddConstraint`. |
| Optimizer | Assume canonical input and select formulations for ABS, MIN/MAX, AVG, `<>`, bilinear products, and other rewrites. Generated rows return through `AddConstraint`. |
| Physical extraction | Read model terms from the left and the bound from the right. It may assert the invariant but must not repair it. |
| Runtime evaluation | Evaluate coefficients, `WHEN`, `PER`, qualifiers, data reducers and row-varying bounds. These are value operations, not shape decisions. |
| Model builder | Accumulate coefficients and build solver-neutral rows. It does no SQL-expression canonicalization. |
| Diagnostics / EXPLAIN | Use preserved source provenance for user-facing text and canonical/rewritten trees for internal views. |

---

## 3. Canonical constraint contract

### 3.1 Input

The input is a **bound** DECIDE constraint tree after scalar subqueries have
been planned. At that point:

- decision variables are `BoundColumnRefExpression`s on `decide_index`;
- variable scope is known;
- scalar-subquery correlation was observed before flattening;
- all expressions have DuckDB types and binder-inserted casts.

Canonicalization must remain pure: it takes the input by const reference and
returns a new tree. This keeps the source and canonical forms independently
available for diagnostics and layered `EXPLAIN`.

### 3.2 Tree structure

A canonical tree satisfies these rules:

| ID | Rule |
|---|---|
| **C0** | Ordinary `AND` conjunctions recurse into every child. A `WHEN` or `PER` wrapper recurses into child 0 only; its condition or grouping columns are copied unchanged. Non-comparison leaves are copied unchanged. |
| **C1** | Each DECIDE comparison has at least one decision-bearing term. |
| **C2** | The right side contains no decision-variable reference. |
| **C3** | Every top-level decision-bearing additive term is on the left. Every top-level decision-free additive term is on the right. Data inside a reducer body remains inside that reducer. |
| **C4** | The left and right additive spines contain only `+`, binary `-`, unary `-`, and casts proven safe to distribute. The pass never opens an atom. |
| **C5** | An aggregate constraint contains only decision-bearing reducers and row-invariant decision terms at the top level. A per-row constraint contains no reducers. Unsupported mixtures are rejected at this boundary. |
| **C6** | A factor attached to a decision-bearing reducer has one supported canonical representation and is one value for the whole query. |
| **C7** | Structural tags and source provenance survive rebuilding. The result is a fixed point: `Canon(Canon(c)) == Canon(c)`. |

This is a structural canonical form, not algebraic simplification. The model
builder already accumulates coefficients per solver column, so combining
`2*x + 3*x` into `5*x` is not required here.

### 3.3 Additive decomposition

The pass walks only the additive spine:

```text
a + b       -> (+a, +b)
a - b       -> (+a, -b)
-a          -> (-a)
safe cast   -> descend and reapply the cast to each rebuilt term
anything else -> one atomic term
```

Examples of indivisible atoms include:

- `x`
- `price * x`
- `POWER(x - target, 2)`
- `SUM(x * price)`
- `MAX(x * value)`
- `SUM(x) WHEN condition`

The canonicalizer may ask whether an atom contains a decision reference or a
reducer. It must not expand the atom or change its internal algebra.

### 3.4 Placement

For each additive atom:

```text
references a DECIDE variable -> LEFT
otherwise                    -> RIGHT
```

Crossing the comparison negates the atom. For example:

```sql
demand - SUM(ship) <= cap
```

becomes structurally equivalent to:

```sql
-SUM(ship) - cap <= -demand
```

The data expression is then evaluated as the bound, while the aggregate and
query-wide decision are model terms.

### 3.5 Relation orientation

The canonical relation follows one deterministic policy:

- If the left side already contains a decision term, retain the comparison
  operator and migrate individual atoms by changing their signs.
- If every decision term is on the right, swap the two complete sides and flip
  the comparison operator.

Therefore:

```sql
5 >= x
```

canonicalizes to:

```sql
x <= 5
```

The contract is **semantic fidelity**, not preservation of the operator token
at the canonical-tree root. User-written orientation belongs to source
provenance. This replaces the old K4 claim that the operator never changes,
which did not match `CanonicalizeComparison` or diagnostic output.

### 3.6 Cast descent

A cast may be distributed over an additive spine only when the conversion is
provably exact for every input value. DuckDB accepting an implicit cast is not
enough.

The exactness policy should be an explicit allow-list, including cases such as:

- identical types;
- integer to an integer type with at least as much range;
- `FLOAT` to `DOUBLE`;
- DECIMAL to DECIMAL when both fractional capacity and integral capacity are
  preserved;
- integer to DECIMAL only when the target can represent the entire source
  range.

Lossy or uncertain cases remain atomic. In particular, `BIGINT -> DOUBLE` is
not exact above `2^53` and must not be distributed.

### 3.7 Reducer scales

A factor outside a decision-bearing reducer is part of the atom:

```sql
2 * SUM(x)
SUM(x) * 2
SUM(x) / 2
```

The canonical representation keeps the factor outside the reducer. Moving it
inside is wrong for order statistics under a negative factor, and a query-wide
factor may not be known until execution.

A legal factor must be one value for the whole query:

- literals and foldable expressions are legal;
- uncorrelated scalar subqueries are legal;
- arithmetic made entirely from query-wide values is legal;
- row-varying columns and correlated subqueries are illegal;
- a decision-bearing factor is not a scale; it is a product of decisions.

Nested scales such as `2 * (3 * SUM(x))` and `(SUM(x) / 2) / 3` use a composed
query-wide scale. Typed operations must be preserved in their written order;
the canonicalizer must not reassociate division or pre-evaluate a query-wide
subquery. These shapes currently fall through to a late physical error, which
is a defect: accepted nested scales must reach the same scale representation as
their single-factor equivalents.

### 3.8 Source provenance and tags

Canonical and source form are different concerns. The canonical tree exists
for downstream execution; diagnostics must still know what the user wrote.

At minimum, preserve:

- the original clause identity and spelling;
- whether a value came from an uncorrelated or correlated scalar subquery;
- whether a rebuilt RHS is wholly query-wide or contains row-varying data;
- optimizer mechanism tags such as structural, ABS, MIN/MAX and `<>` tags;
- `WHEN`, `PER` and qualifier ownership.

Provenance must be side-agnostic. `x <= (SELECT 5)` and
`(SELECT 5) >= x` are the same shared cap after canonicalization and must
produce the same actionable diagnosis. Internal names such as `SUBQUERY` must
never appear in suggested SQL.

---

## 4. What the canonicalizer does not do

The canonicalizer does not:

- expand products or powers;
- combine like terms;
- fold constants;
- rewrite AVG, ABS, MIN/MAX, `<>`, bilinear products, `norm()` or `IN`;
- look inside reducer bodies;
- evaluate data, reducers, `WHEN`, `PER`, or qualifiers;
- choose a solver formulation;
- canonicalize objectives.

Grammar repair also does not belong here. The surviving
`SimplifyDecideConstraints` only repairs `A AND B WHEN c` parse association and
should ultimately be replaced by parser/transform-layer ownership rather than
expanded into another constraint-shaping pass.

---

## 5. Current implementation state

### Landed

- `DecideCanonicalizer` exists and is called for user-written and
  optimizer-generated constraints.
- Decision-bearing atoms are moved left and decision-free atoms right.
- The all-decision-on-right mirror case swaps sides and flips the relation.
- Widening-cast descent and scale peeling exist.
- Query-wide scalar decisions may appear beside reducers.
- Data reducers on the RHS are evaluated per group and by reducer kind.
- Sign-aware ABS and composed MIN/MAX analysis handles migrated negative terms.
- The five former duplicate sites are gone:
  - parsed aggregate RHS hoisting;
  - parsed comparison simplification;
  - the binder's side flip;
  - the physical per-row repartition;
  - `lhs_offset_expr` and its runtime subtraction.
- `LogicalDecide::AddConstraint` canonicalizes generated constraints.
- The physical per-row path has a defensive check for decision variables on
  the RHS.

### Blocking defects and gaps

1. **Lossy cast distribution can produce a wrong answer.**
   `IsWideningNumericCast` currently treats any implicit numeric cast as safe.
   `BIGINT -> DOUBLE` disproves that above `2^53`.

2. **Reversed scalar-subquery bounds lose provenance.**
   `x <= (SELECT 5)` diagnoses as a shared editable cap, but
   `(SELECT 5) >= x` can report `x <= SUBQUERY + 5` after side swapping.

3. **Nested reducer scales are neither normalized nor rejected at the owning
   boundary.** They fail later in physical extraction.

4. **C5/K3 validation remains downstream.**
   `PhysicalDecide::ExtractAggregateConstraintTerms` still issues user-facing
   errors for non-aggregate top-level terms.

5. **The invariant is conventional, not structural.**
   `VerifyCanonical()` does not exist, and optimizer passes can still mutate
   `decide_constraints` in place.

6. **The `PER` gate has a homogeneity hole.**
   A data-only reducer on the left can make a per-row decision constraint look
   aggregate, allowing a meaningless `PER` through.

7. **`FixedLinearLhsOffset` still looks generic but always sums data-only
   reducer-body terms.** It must either be explicitly SUM-specific with a guard
   or be routed through reducer-aware evaluation when another meaning is
   reachable.

8. **Objective normalization remains separate unfinished work.**
   `SimplifyDecideObjective`, `objective_constant_offset`, and objective grammar
   repair keep the old symbolic path alive.

---

## 6. Completion plan

Do the work in this order. Correctness and the contract come before structural
cleanup.

### Step 1 — lock this contract and add failing tests (landed 2026-08-12)

Add adversarial coverage before changing implementation:

- `BIGINT -> DOUBLE` around and above `2^53`;
- exact and inexact DECIMAL cast cases;
- forward and reversed uncorrelated scalar-subquery diagnostics;
- correlated subqueries on both sides;
- nested multiplication and division around reducers;
- aggregate/per-row homogeneity and variable-scope combinations;
- the data-only-reducer `PER` case;
- idempotence for side swaps, casts, scales, `WHEN` and `PER`.

Tests must check the result or independent oracle, emitted model, error stage and
message, and diagnostic suggestion where relevant. Binding successfully is not
evidence of correctness.

The contract is now pinned by focused tests:

- `test_canonicalize_cast.py` compares lossy BIGINT/DECIMAL cast-lid results
  with direct DuckDB evaluation around `2^53`, with exact DECIMAL widening as a
  control;
- `test_query_diagnostics_relation.py` requires forward and reversed
  uncorrelated subquery bounds to emit the same editable SQL suggestion, while
  `test_canonicalize_side_agnostic.py` checks correlated bounds on both sides;
- `test_canonicalize_scale.py` chooses the preferred accepted-shape contract:
  composed query-wide multiplication and division around reducers are
  supported, not rejected;
- `test_canonicalize_homogeneity.py` pins scalar, row and entity scope behavior
  plus the data-only-reducer `PER` hole at the binder/planning boundary;
- `test_canonicalize_idempotence.py` compares byte-identical emitted models for
  non-canonical and fixed-point spellings covering side swaps, exact casts,
  scales, `WHEN` and `PER`.

The red tests are intentional at this step. They reproduce the cast wrong
answers, reversed-subquery provenance loss, late nested-scale failures, and
downstream/missing homogeneity validation that Steps 2-5 own.

### Step 2 — make cast descent exact

Replace `CastRules::ImplicitCast >= 0` as the general definition of widening.
Implement and test the explicit exactness policy in §3.6. Leave an uncertain
cast atomic; never distribute it merely because DuckDB permits it implicitly.

This is the first implementation step because it closes a demonstrated wrong
answer.

### Step 3 — make provenance side-agnostic

Collect scalar-subquery provenance from both comparison sides before
flattening. Carry semantic facts through canonical rebuilding rather than
encoding the original position as "RHS".

Classify the complete rebuilt bound:

- wholly query-wide -> shared scalar bound;
- contains row-varying data -> per-row/per-group data.

Forward and reversed spellings must produce equivalent diagnostic subjects and
applicable SQL edits.

### Step 4 — implement reducer-scale totality

Implement the composed nested-scale contract in §3.7 while preserving typed
evaluation and operation order.

In both cases, every accepted scale has one shape understood by optimizer and
physical consumers.

### Step 5 — centralize homogeneity validation

Add `ValidateCanonicalTree` or equivalent beside the canonicalizer. Give it the
variable-scope and subquery metadata required to distinguish row, entity and
query-wide values.

It owns:

- aggregate versus per-row classification;
- legal top-level atoms for each category;
- query-wide scalar decisions beside reducers;
- row-scoped decisions beside reducers;
- illegal scale factors;
- the real aggregate predicate used by `PER`.

Physical extraction may keep defensive internal assertions, but a user query
must not first discover a structural error there.

### Step 6 — enforce the invariant

Implement `VerifyCanonical()` and run it:

- after canonicalizing user constraints;
- inside or immediately after `LogicalDecide::AddConstraint`;
- after optimizer rewriting and before physical planning;
- optionally at `PhysicalDecide::AnalyzeConstraint` as a final debug guard.

It verifies C0-C7 that are observable from the output tree. Relational source
provenance is verified by tests.

Audit every optimizer mutation of `decide_constraints`. A pass that can change
placement must rebuild through `AddConstraint` or a canonicalizing replacement
API. Placement-preserving in-place rewrites must be documented and checked by
the post-optimizer verifier.

### Step 7 — close downstream shape assumptions

- Binder: classify and type-check without moving terms.
- Optimizer: match only canonical scale and placement forms.
- Physical extraction: consume rather than repair.
- `FixedLinearLhsOffset`: make SUM ownership explicit and fail loudly on any
  unsupported reachable meaning.
- Diagnostics: render from provenance rather than internal flattened names.

Run a stale-symbol and stale-comment sweep after the code changes. Tests must
not describe deleted fallbacks or the removed parsed comparison simplifier as
current behavior.

### Step 8 — resolve objectives as a separate pass

Do not extend comparison canonicalization to objectives. Define an objective
normalization contract separately:

1. move objective `WHEN` grammar repair to the parser/transform layer;
2. determine which objective structural normalization is actually required;
3. replace or remove `SimplifyDecideObjective` and SymbolicC++;
4. preserve objective constants explicitly if exact objective-value reporting
   will need them;
5. remove `objective_constant_offset` only after its replacement is settled.

Constraint canonicalization can be declared complete before this step. The
whole canonicalization issue can close only when this objective work is either
landed or moved into a separately named, authoritative task.

### Step 9 — reconcile documentation and trackers

- Update the pipeline documents to this ownership boundary.
- Remove stale `NEUTRAL` and deleted-fallback descriptions.
- Correct test module docstrings.
- Reconcile references to the deleted `07_issues/bugs/done.md`.
- Remove the canonicalization entry from
  `07_issues/code_quality/todo.md` when the definition of done below is met.
- Leave unrelated ABS, diagnostics and bound-absorption bugs in their own
  trackers; they are not canonicalizer completion work unless a change here
  touches them.

---

## 7. Definition of done

### Constraint canonicalization is done when

- Every accepted bound DECIDE constraint reaches one deterministic structural
  form.
- Every unsupported form is rejected at the canonicalization boundary with a
  concise, actionable SQL-level error.
- No downstream stage moves terms across a comparison.
- No downstream stage decides aggregate-versus-per-row homogeneity for the
  first time.
- Cast distribution is provably exact.
- Forward and reversed forms preserve equivalent source provenance and
  diagnostics.
- Reducer scales are either fully canonicalized or explicitly rejected by the
  documented contract.
- User-written and optimizer-generated constraints use the same boundary.
- `VerifyCanonical()` passes after optimizer rewriting.
- The pass is pure and idempotent.
- No internal `SUBQUERY` names or physical-extractor shape errors escape to
  users for accepted syntax.
- The full suite and targeted adversarial tests pass on both solver backends
  where applicable.
- The golden model corpus is unchanged except for individually explained
  correctness fixes.
- `git diff --check` passes and current documentation matches current code.

### The full canonicalization matter is done when

- constraint canonicalization satisfies the list above;
- objective normalization and grammar repair are either completed or moved to
  a separate authoritative task with no constraint-canonicalization dependency;
- `context/descriptions/07_issues/code_quality/todo.md` has no remaining
  canonicalization entry;
- this document contains no open implementation step.

Passing the existing suite is necessary but not sufficient. At the start of
this closeout the tree passed 1,059 tests and all 80 golden models, while still
containing the lossy-cast wrong answer and reversed-subquery diagnostics defect.

---

## 8. Brief implementation handoff

For a new agent taking over:

- Start with Step 1 and work in order; do not begin with `VerifyCanonical()`.
- Preserve the user's existing worktree and treat this document as the current
  contract rather than reconstructing the deleted historical phases.
- Add each adversarial regression test before its fix, then run the focused
  canonicalizer tests and golden diff after every step.
- Keep transformation and validation at the planning boundary; do not restore a
  binder, optimizer or physical fallback.
- Stop and report any new wrong-answer case before expanding the refactor's scope.

---

## 9. Verification commands

Run from the repository root:

```bash
make release

test/decide/.venv/bin/python3 -m pytest \
  test/decide/tests/test_canonicalize_cast.py \
  test/decide/tests/test_canonicalize_scale.py \
  test/decide/tests/test_canonicalize_sign.py \
  test/decide/tests/test_canonicalize_side_agnostic.py \
  -v

./test/decide/golden/capture.sh /tmp/canonicalize-after.dump
diff -u test/decide/golden/baseline.dump /tmp/canonicalize-after.dump

make decide-test
git diff --check
```

For diagnostics that must continue after a failing DECIDE statement, use a
multi-statement stdin script on one connection. CLI `-c` stops at the first
error and cannot read `decide_diagnostics()` afterward.

---

## 10. Source map

| Concern | Primary location |
|---|---|
| Canonical transformation | `src/planner/decide/decide_canonicalizer.cpp` |
| Canonicalizer contract | `src/include/duckdb/planner/decide/decide_canonicalizer.hpp` |
| User-written call site and subquery provenance | `src/planner/binder/query_node/plan_select_node.cpp` |
| Constraint binding and `PER` gate | `src/planner/expression_binder/decide_constraints_binder.cpp` |
| Generated-constraint entry point | `src/planner/operator/logical_decide.cpp` |
| Optimizer mutations and formulations | `src/optimizer/decide/decide_optimizer.cpp` |
| Physical extraction and invariant guard | `src/execution/operator/decide/physical_decide.cpp` |
| Coefficient accumulation | `src/decidb/utility/ilp_model_builder.cpp` |
| Canonical model corpus | `test/decide/golden/` |
| Canonicalizer behavior tests | `test/decide/tests/test_canonicalize_*.py` |
| Open code-quality tracker | `context/descriptions/07_issues/code_quality/todo.md` |
| Open correctness bugs | `context/descriptions/07_issues/bugs/todo.md` |
