# DECIDE constraint canonicalization

**Status (2026-08-13):** the five duplicate constraint-shaping paths and Step 1's
adversarial contract tests are landed. Steps 2, 3 and 4 are complete: casts have one
policy at one site (§3.6), scalar-subquery provenance is classified from the
complete canonical bound rather than its original side, and nested reducer scales
compose to one factor (§3.7). Resume at Step 5; canonicalization as a whole is
**not complete**.

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

### 3.6 Casts

A DECIDE model carries **one numeric domain, `DOUBLE`** — the solver interface is
`double` throughout (`solver_input.hpp`), so every bound and coefficient lands there
regardless of the types the expression tree held. That fact decides what a cast means
here, and the policy lives in one place: `src/decidb/utility/decide_cast_policy.cpp`.

A cast on a decision-bearing path is exactly one of two things:

- **Representation** — the same value in a different container. Everything the binder
  inserts to reconcile two comparison sides is this. It is safe to look through, and
  every consumer does so unconditionally.
- **Computation** — a different value. The target is integral while the source is
  fractional (`CAST(x AS INTEGER)` is `round(x)`, a step function), or a `DECIMAL`
  target has a smaller scale. These are **rejected** at this boundary with an
  actionable SQL-level message.

`DecidePreservesResolution(from, to)` is the single predicate separating them, and
`DecideCanonicalizer::ValidateDecisionCasts` is the single enforcement point. Anything
into `DOUBLE` preserves resolution by definition — it is the edge of what DECIDE
models, not a loss inside it.

**Why this is a structural rule and not a per-site check.** A census before this work
found 45 cast-unwrapping sites, 42 of them independent, only 12 guarded — and cast
bugs kept recurring in whichever site was still unguarded. Guarding each site does not
converge. Establishing the invariant once does: because no query carrying a
value-changing decision cast leaves planning, an unguarded peel downstream cannot be
wrong, and a *newly written* one is right by default.

A cast over decision-free **data** is outside this rule entirely. It is an ordinary
value computation the executor performs, and peeling it would change a coefficient —
`x <= CAST(1.6 AS INTEGER)` is a bound of 2, not 1.6. `UnwrapDecideCasts` therefore
stops at any resolution-reducing cast, which makes one helper correct on both paths.
Rendering is the one job that peels unconditionally, via the separately named
`StripCastsForDisplay` / `DecideDisplayString`, because its output is a label rather
than a value.

Distributing a cast over an additive spine uses the same predicate: only a
resolution-preserving cast may be pushed onto individual terms.

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

Nested scales such as `2 * (3 * SUM(x))` and `(SUM(x) / 2) / 3` compose into a
single query-wide scale. Peeling runs to exhaustion, gathering factors by role:
multipliers compose into `M`, divisors into `D`, and the emitted factor is `M`,
`D`, or `M / D`. A nested scale therefore reaches exactly the representation its
single-factor equivalent does — one level, one spelling — which is what lets every
downstream consumer keep its single-level match.

The canonicalizer must not pre-evaluate a query-wide subquery: composition builds
an expression, so `(SELECT a) * ((SELECT b) * SUM(x))` emits the factor
`(SELECT a) * (SELECT b)` and the values still arrive at execution.

Composition **does** reassociate. `(SUM(x) / 2) / 3` emits `SUM(x) / (2 * 3)`,
which in double arithmetic is not bit-identical to dividing twice. This is a
deliberate accepted cost, decided on 2026-08-13: the model carries one numeric
domain (`DOUBLE`, §3.6) end to end, the discrepancy is last-ULP (~1e-16 relative,
measured at ~35% of random triples), and solver tolerances are 1e-6 to 1e-9 — some
ten orders of magnitude coarser. The golden dump formats with `%.10g`, so a
composed scale is not even observable there. The alternative — a per-factor
"is folding safe here?" predicate — is the per-site pattern §3.6 exists to remove.

The known limit is magnitude: composing factors near the double range can overflow
to `inf` or underflow to `0` where sequential application would not. DeciDB does
not special-case this, on the same reasoning every numeric system applies.

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
- Widening-cast descent and scale peeling exist. Peeling is total: nested
  multipliers and divisors compose into one factor, so `2 * (3 * SUM(x))`,
  `(SUM(x) / 2) / 3` and mixed nestings rebuild at a single level (§3.7).
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
- Casts have one policy in one module (`decide_cast_policy.cpp`) and one
  enforcement point (`ValidateDecisionCasts`). Resolution-preserving casts are
  transparent everywhere; value-changing ones over a decision are rejected with an
  actionable message. See §3.6.
- Explicit decision-bearing casts are accepted by parsed-expression shape
  classification. Decision-free casts remain on DuckDB's ordinary expression
  evaluation path, and are never peeled by the shared unwrap helper.
- Scalar-subquery provenance is collected from both comparison sides before
  flattening. Uncorrelated values remain query-wide through rebuilding;
  correlated values remain row-varying.
- The canonicalizer classifies the complete rebuilt RHS. Forward, reversed and
  additive query-wide bounds share one diagnostic edit, while a bound with any
  row-varying component remains data-backed.
- Physical evaluation consumes the canonical RHS classification. Diagnostic
  rendering never exposes DuckDB's internal `SUBQUERY` placeholder.

### Blocking defects and gaps

1. **C5/K3 validation remains downstream.**
   `PhysicalDecide::ExtractAggregateConstraintTerms` still issues user-facing
   errors for non-aggregate top-level terms.

2. **The invariant is conventional, not structural.**
   `VerifyCanonical()` does not exist, and optimizer passes can still mutate
   `decide_constraints` in place.

3. **The `PER` gate has a homogeneity hole.**
   A data-only reducer on the left can make a per-row decision constraint look
   aggregate, allowing a meaningless `PER` through.

4. **`FixedLinearLhsOffset` still looks generic but always sums data-only
   reducer-body terms.** It must either be explicitly SUM-specific with a guard
   or be routed through reducer-aware evaluation when another meaning is
   reachable.

5. **A decision-free NULL bound is not rejected at the planning boundary.**
   `x + 3 <= CAST(NULL AS DOUBLE)` reaches physical bound absorption and throws
   an internal `GetValueInternal` error. Canonical validation must reject it
   with an actionable SQL-level message before physical planning.

6. **Objective normalization remains separate unfinished work, and it carries a
   live wrong answer.** `SimplifyDecideObjective`, `objective_constant_offset`,
   and objective grammar repair keep the old symbolic path alive. Because that
   path erases `CastExpression` nodes before binding,
   `MAXIMIZE SUM(CAST(x AS INTEGER) * w)` silently optimizes `SUM(x * w)` —
   the same cast a constraint rejects. Owned by Step 8, item 6.

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

The Step 2 cast tests, Step 3 provenance tests and Step 4 nested-scale tests are
now green. The remaining intentional red tests reproduce the downstream and
missing homogeneity validation that Step 5 owns.

### Step 2 — one cast policy at one site (complete 2026-08-13)

Superseded the earlier preimage-inversion approach. That version modelled every
type-lossy cast exactly, by inverting it: it fixed the original three `<=` failures
but left the same bug reachable elsewhere and made ordinary queries far slower.

What it cost, measured before replacement:

- `CAST(x+v AS DOUBLE) + CAST(y+v AS DOUBLE) <= K` still returned a wrong answer —
  the lowering only fired when the cast wrapped the *complete* side;
- `SUM(x) <= <decimal column>` ran **120x slower** (4.5 s vs 0.04 s at 10k rows,
  44 s at 100k), because `HUGEINT -> DECIMAL(38,1)` is lossy by *type* and so ran a
  full per-row lattice search — for a `SUM(x)` that cannot exceed a few thousand;
- every such constraint also carried two rigid rows bounding `SUM(x)` to +/-1e37.

DuckDB solves the same problem with one predicate
(`BoundCastExpression::CastIsInvertible`) in one rewrite rule, does no preimage
inversion at all, and makes the cast vanish before anything downstream sees it. The
one place it expands a comparison into an interval (`TIMESTAMP -> DATE`) is a
hardcoded one-off. What landed follows that shape:

1. `decide_cast_policy.{hpp,cpp}` is the single home: `DecidePreservesResolution`,
   `UnwrapDecideCasts`, and the display-only `StripCastsForDisplay` /
   `DecideDisplayString`. It replaced `numeric_cast_preimage.cpp` (430 lines).
2. `DecideCanonicalizer::ValidateDecisionCasts` enforces §3.6 at the boundary.
3. The three byte-identical `UnwrapCasts` helpers collapsed to one; the 11
   `IsExactDecideNumericCast` guards in `physical_decide.cpp` were deleted as dead
   under the invariant.
4. `EvaluatedConstraint` lost eight cast fields, `DecideConstraint` three, and the
   preimage lowering block plus its `cast_ne_interval` branches through the Big-M
   `<>` expansion are gone. `source_clause_id` was kept — it is a general
   clause-identity improvement, not cast machinery.

Verification:

- all 80 golden models and their results are **byte-identical to the committed
  pre-Step-2 baseline** — the +/-1e37 rows are gone and nothing else moved;
- `make decide-test`: 1107 passed, 7 failed — exactly the pre-existing set (six
  Step 4/5 pins plus the NULL-bound gap), no new failures;
- `unittest "[decidb]"`: 393 assertions pass;
- the 120x case is back to 0.037 s, identical to the same query without a cast;
- `git diff --check` clean.

Two consequences recorded rather than hidden. Past `2^53` a model and row-wise SQL
can disagree, which is now a documented limit with its own characterization test
rather than an accident. And removing the preimage path unmasked a pre-existing,
unrelated gap — an infinite bound in a rebuilt additive RHS is rejected while the
same bound alone is accepted — filed in `07_issues/bugs/todo.md`.

### Step 3 — make provenance side-agnostic (complete 2026-08-12)

Implemented as semantic value and bound classification rather than another
positional special case:

1. `PlanSelectNode` records scalar subqueries from both comparison sides before
   flattening. Uncorrelated flattened values carry `QUERY_WIDE_VALUE_TAG`;
   correlated ones carry `ROW_VARYING_SUBQUERY_TAG`.
2. `DecideCanonicalizer::FinalizeBoundProvenance` removes any stale root
   classification, examines the complete canonical RHS, and stamps
   `QUERY_WIDE_BOUND_TAG` only when every component is query-wide.
3. The optimizer-generated entry point honors explicit value provenance but
   never guesses that an arbitrary column is query-wide.
4. Physical evaluation consumes the root classification through
   `rhs_is_shared_scalar`; solver provenance calls the resulting elastic shape
   `SHARED_SCALAR` to cover literals and other query-wide values uniformly.
5. Mixed and correlated subquery bounds remain data-backed. Their diagnostics
   use an evaluated numeric fallback rather than leaking `SUBQUERY` or an
   internal DECIDE tag into suggested SQL.

Forward/reversed direct bounds, rebuilt additive bounds, multiple query-wide
components, mixed data bounds, correlated bounds and model idempotence are
covered on both backends.

Verification (Step 3, at the time it landed):

- `make release` succeeds;
- all 149 `test_query_diagnostics_relation.py` cases pass on their configured
  backends;
- the focused Step 3 plus idempotence selection passes all 21 cases;
- the five-file canonicalizer run passes 71 of 74 cases; its three failures are
  the two Step 4 nested-scale pins and the Step 5 row-scope validation pin;
- all 80 golden models and result/error lines are byte-identical to the
  immediate pre-Step-3 capture;
- the full suite passes 1,108 of 1,115 cases. Six failures are the open Step 4/5
  contract tests; the seventh is the NULL-bound planning-validation gap listed
  above. No Step 3 test failed, and the emitted model corpus did not change.

### Step 4 — implement reducer-scale totality (complete 2026-08-13)

The fix turned out to live entirely in `PeelScale`, with **no downstream edit**.
`PhysicalDecide::AsScaledAggregate` re-derives the factor from the tree rather than
reading `Atom.scale`, and it matches only a single level (`factor * AGG` /
`AGG / factor`). Peeling one level off `2 * (3 * SUM(x))` rebuilt the identical
tree, so physical failed to match and errored late. Peeling to exhaustion and
composing rebuilds `(2 * 3) * SUM(x)`, which the existing matcher already accepts.

What landed:

1. `PeelScale` loops instead of testing once, gathering factors by role —
   multipliers into `M`, divisors into `D` — and unwrapping resolution-preserving
   casts between levels (safe because `ValidateDecisionCasts` ran first, and
   physical unwraps identically).
2. The emitted factor is `M`, `D`, or `M / D`, so mixed nestings collapse to one
   multiplication rather than needing a numerator and denominator slot.
3. Each nesting level is judged separately, so an illegal inner factor names
   itself: `2 * (w * SUM(x))` reports `w`, not `2`.
4. `Atom::scale` became an owning `unique_ptr<Expression>` — a composed factor is
   a new node and cannot be borrowed from the input tree. `Atom` is now move-only,
   which made the two copy sites in the partition loop explicit moves.

Composition reassociates in double arithmetic; §3.7 records why that is accepted.

Verification:

- the two nested-scale contract tests pass; the full scale file is 23/23;
- three-level nesting, both mixed multiply/divide orders, and composed query-wide
  subquery factors were each checked against hand-derived bounds;
- illegal inner factors (row-varying and decision-bearing) still reject at the
  binder with the message naming the inner factor;
- all 80 golden models **and** their results are byte-identical to baseline;
- `make decide-test`: 1109 passed, 5 failed — down from 7; the remaining 5 are the
  four Step 5 homogeneity pins plus the Step 5 NULL-bound gap;
- `git diff --check` clean.

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
- rejection of decision-free bounds that evaluate to NULL, before physical
  bound absorption or runtime evaluation.

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
5. remove `objective_constant_offset` only after its replacement is settled;
6. **the objective cast case is still open and this step owns it.** A rounding
   cast over a decision is rejected in a constraint but silently dropped from a
   `SUM` objective — `MAXIMIZE SUM(CAST(x AS INTEGER) * w)` optimizes
   `SUM(x * w)` instead. The cause is item 3: `SimplifyDecideObjective` erases
   `CastExpression` nodes at the parsed level, before binding, so the
   `ValidateDecisionCasts` call already placed on the bound objective in
   `plan_select_node.cpp` never sees them (`MIN`/`MAX` forms are refused earlier
   by the binder's shape check). No new check is needed — removing the symbolic
   rewrite closes this automatically. **Do not add a cast check inside the
   symbolic layer**; that rebuilds the per-site pattern §3.6 exists to remove.
   Filed in `07_issues/bugs/todo.md`.

Constraint canonicalization can be declared complete before this step. The
whole canonicalization issue can close only when this objective work is either
landed or moved into a separately named, authoritative task — and item 6 means
that task carries a live wrong-answer case, not only cleanup.

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
- Casts have one policy and one enforcement point: resolution-preserving casts are
  transparent, value-changing casts over a decision are rejected, and casts over data
  are left alone.
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
this closeout the tree passed 1,059 tests and all 80 golden models despite the
then-undetected lossy-cast wrong answer and reversed-subquery diagnostics defect.

---

## 8. Brief implementation handoff

For a new agent taking over:

- Start at Step 5. Steps 1-4 are complete; do not reopen the cast policy, the
  side-agnostic provenance path, or reducer-scale composition while centralizing
  homogeneity validation.
- Preserve the user's existing worktree and treat this document as the current
  contract rather than reconstructing the deleted historical phases.
- Casts are settled: one predicate (`DecidePreservesResolution`), one enforcement
  point (`ValidateDecisionCasts`), and a deliberate separation between the semantic
  `UnwrapDecideCasts` and the display-only `StripCastsForDisplay`. Do not add a
  per-site cast guard; if a site needs one, the invariant is what is broken.
  `source_clause_id` is retained and is not cast machinery.
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
  test/decide/tests/test_canonicalize_homogeneity.py \
  test/decide/tests/test_canonicalize_idempotence.py \
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
| Cast policy and unwrapping | `src/decidb/utility/decide_cast_policy.cpp` |
| Cast-policy interface | `src/include/duckdb/decidb/decide_cast_policy.hpp` |
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
