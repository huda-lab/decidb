# Stage 04 — Canonicalizer

> One planning boundary decides the structural shape of every DECIDE constraint
> **and objective**. Downstream stages consume that shape and never re-decide it.

This document is the authoritative specification for that shape. It describes
current code.

**Key source files**

- `src/planner/decide/decide_canonicalizer.cpp`
- `src/include/duckdb/planner/decide/decide_canonicalizer.hpp`

---

## 1. Ownership

The boundary is two operations:

1. **Canonicalize** a bound tree into the structural form in §3.
2. **Validate** that the result is a supported per-row or aggregate constraint.

Keeping transformation and user-facing validation as separate operations is fine.
The invariant is that both happen at one boundary, before optimization, and that
no later layer partitions a comparison again.

One boundary, three call sites, and there must not be a fourth:

| Entry point | Covers |
|---|---|
| `Binder::CreatePlan`, after `PlanSubqueries` | Constraints **and the objective** as written by the user |
| `LogicalDecide::AddConstraint` | Constraints synthesized by `DecideOptimizer` |
| `LogicalDecide::SetObjective` | Objectives rewritten by `DecideOptimizer` |

Assigning `decide_constraints` or `decide_objective` directly bypasses both
canonicalization and verification.

### Why it runs after binding

A decision variable is *exactly* a `BoundColumnRefExpression` on `decide_index` —
that identity does not exist on the parsed tree. The pass also needs the flattened
form `PlanSubqueries` produces. Both facts pin it to this point.

### Why it is total

The pass **never opens a term algebraically**. It decomposes each side additively
and asks one question of each resulting term — "does it reference a decision
variable?" — then moves terms across the relation. A quadratic `POWER(x-t,2)`, a
composed `MAX(x*v)` and a filtered `SUM(x) WHEN c` are all just decision-bearing
terms here, indistinguishable from `x`.

That restriction is the whole design. A pass that may decline can never be the
single home for anything: every consumer downstream then has to handle both "it
ran" and "it declined", and the declined branch is where duplicate
implementations come from. That is exactly how canonicalization once came to be
spread across five sites.

---

## 2. Place in the pipeline

```text
bind names, types, scopes, reducers and subqueries
        |
        v
PlanSubqueries and retain query-wide/correlated provenance
        |
        v
canonicalize + validate constraints AND objective   <-- single shape boundary
        |
        v
LogicalDecide  ---> VerifyCanonical
        |
        v
optimizer rewrites; generated rows return through AddConstraint / SetObjective
        |
        v
VerifyCanonical at physical-plan entry
        |
        v
physical extraction and runtime RHS/group evaluation
```

| Stage | Responsibility once canonicalization owns shape |
|---|---|
| Parser | Parse with the intended association and retain NORM/IN source structure. Does not move comparison terms. |
| Binder | Resolve names, types, scopes, reducers, subquery correlation. Determines *whether* a comparison is a DECIDE constraint; does not flip or repartition it. |
| Canonicalizer | Put every accepted bound comparison into §3 and reject unsupported mixtures once. |
| Logical plan | Store canonical constraints. New ones enter only through `AddConstraint`. |
| Optimizer | Assume canonical input; lower NORM/IN and select other formulations. Generated rows return through `AddConstraint`. |
| Physical extraction | Read model terms from the left and the bound from the right. May assert the invariant; must not repair it. |
| Runtime evaluation | Evaluate coefficients, `WHEN`, `PER`, qualifiers, data reducers, row-varying bounds. Value operations, not shape decisions. |
| Model builder | Accumulate coefficients and build solver-neutral rows. No SQL-expression canonicalization. |
| Diagnostics / EXPLAIN | Source provenance for user-facing text; canonical/rewritten trees for internal views. |

---

## 3. The canonical contract

### 3.1 Input

A **bound** DECIDE tree after scalar subqueries have been planned. At that point
decision variables are `BoundColumnRefExpression`s on `decide_index`, variable
scope is known, correlation was observed before flattening, and every expression
carries DuckDB types and binder-inserted casts.

The pass is **pure**: it takes its input by const reference and returns a new
tree. This is load-bearing, not incidental — it keeps the as-written and canonical
forms simultaneously live at each call site, which is what makes layered `EXPLAIN`
a rendering job rather than a re-plumbing job.

### 3.2 Tree structure

| ID | Rule |
|---|---|
| **C0** | Ordinary `AND` conjunctions recurse into every child. A `WHEN` or `PER` wrapper recurses into child 0 only; its condition or grouping columns are copied unchanged. Non-comparison leaves are copied unchanged. |
| **C1** | Each DECIDE comparison has at least one decision-bearing term. |
| **C2** | The right side contains no decision-variable reference. |
| **C3** | Every top-level decision-bearing additive term is on the left; every top-level decision-free additive term is on the right. Data inside a reducer body stays inside that reducer. |
| **C4** | Both additive spines contain only `+`, binary `-`, unary `-`, and binder-generated casts over decision algebra. Data casts are atoms. The pass never opens an atom or distributes a cast. |
| **C5** | An aggregate constraint contains only decision-bearing reducers and row-invariant decision terms at the top level. A per-row constraint contains no reducers. Unsupported mixtures are rejected here. |
| **C6** | A factor attached to a decision-bearing reducer has one supported canonical representation and is one value for the whole query. |
| **C7** | Structural tags and source provenance survive rebuilding. The result is a fixed point: `Canon(Canon(c)) == Canon(c)`. |

This is a **structural** canonical form, not algebraic simplification. The model
builder already accumulates coefficients per solver column, so `2*x + 3*x` is not
combined into `5*x` here.

### 3.3 Additive decomposition

```text
a + b       -> (+a, +b)
a - b       -> (+a, -b)
-a          -> (-a)
binder cast over decision algebra -> descend without preserving the wrapper
anything else -> one atomic term
```

Indivisible atoms include `x`, `price * x`, `POWER(x - target, 2)`,
`SUM(x * price)`, `MAX(x * value)`, `SUM(x) WHEN condition`.

### 3.4 Placement

```text
references a DECIDE variable -> LEFT
otherwise                    -> RIGHT
```

Crossing the comparison negates the atom, so

```sql
demand - SUM(ship) <= cap
```

becomes structurally equivalent to

```sql
-SUM(ship) - cap <= -demand
```

There used to be a third placement case, `NEUTRAL`, for a data-only reducer: it
carries no decision so it need not be left, but it collapses rows to one value so
it is a legitimate bound and need not be right either. That was scaffolding for a
right-hand side that could not evaluate one. The RHS evaluator exists now, so the
rule is the real one — decisions left, data right — rather than a half-rule that
left data floating on both sides.

### 3.5 Relation orientation

- Left already bears a decision → keep the operator, migrate atoms by flipping
  their signs.
- Every decision is on the right → swap the complete sides and flip the operator.

So `5 >= x` canonicalizes to `x <= 5`. The contract is **semantic fidelity**, not
preservation of the operator token; user-written orientation belongs to source
provenance.

### 3.6 Casts and DOUBLE ingress

A solver model has one numeric domain, `DOUBLE`. That does **not** license erasing
SQL casts and evaluating intermediates as DOUBLE. A data cast is a value
operation: `CAST(1.6 AS INTEGER)` is 2, and replacing it with 1.6 changes a bound
or a coefficient.

Three rules:

1. **User-authored casts over decision algebra are unsupported.** Before any DECIDE
   rewrite or DuckDB binding can obscure authorship,
   `ValidateDecideNoExplicitDecisionCasts` rejects `CAST`, `TRY_CAST` and `::`
   when the cast's child subtree contains a decision reference. Target type is
   irrelevant, so `CAST(x AS DOUBLE)` is rejected too. This covers `SUCH THAT` and
   the objective, not the post-solve `SELECT` projection.
2. **Data-only casts keep normal DuckDB semantics.** They survive canonicalization,
   coefficient extraction, objective normalization, bound absorption and plan-time
   inspection intact. The complete typed expression is evaluated first; only its
   result is converted once to DOUBLE at solver ingress.
3. **Binder-generated casts over decision algebra are transparent.** Once rule 1 has
   run, a bound cast wrapping a decision-bearing subtree is DuckDB
   type-reconciliation noise. `UnwrapDecideCasts(expr, decide_index)` looks through
   it and stops at a data-only cast.

**This is an authorship boundary, and that is why it cannot be reconstructed
later.** A source cast and a binder cast are both `BoundCastExpression`. Whether a
cast is widening does not answer whether the user wrote it, which is why the
earlier resolution-based predicate was insufficient. `BindOp` also adds fresh
reconciliation casts while rebuilding, so decision-aware cast descent is required
for the pass to remain a fixed point at all.

`StripCastsForIdentity` is separate and may be used only where values are never
read — resolving the binding beneath a cast, for instance. Casts in ordinary SQL,
and casts of solved decisions in the `SELECT` list, are untouched.

### 3.7 Reducer scales

A factor outside a decision-bearing reducer is part of the atom:

```sql
2 * SUM(x)      SUM(x) * 2      SUM(x) / 2
```

The canonical representation keeps the factor **outside** the reducer, and every
consumer reads one of exactly two spellings: `factor * AGG` or `AGG / factor`.

**Why outside.** `MIN`/`MAX` are order statistics, not linear functionals: they
commute with a positive factor only — `MAX(-2x)` is `-2·MIN(x)`, not `-2·MAX(x)`.
Pushing a factor in therefore requires knowing its sign, and a scalar subquery's
sign is not known until the query runs. Leaving it outside makes the sign
irrelevant to *correctness*; it only selects which linearization is cheaper, so an
unknown sign costs performance instead of failing. An earlier version did fold,
swapping MIN/MAX for a negative factor — exact, but it made the sign load-bearing
and had nothing to fall back on when the sign was unknown.

A legal factor must be one value for the whole query:

- literals and foldable expressions — legal;
- uncorrelated scalar subqueries — legal;
- arithmetic made entirely of query-wide values — legal;
- row-varying columns and correlated subqueries — illegal;
- a decision-bearing factor is not a scale at all; it is a product of decisions.

**Peeling runs to exhaustion.** `2 * (3 * SUM(x))` and `(SUM(x) / 2) / 3` each
yield *one* factor: multipliers compose into `M`, divisors into `D`, and the
emitted factor is `M`, `D`, or `M / D`. Partial peeling would produce a term
nothing downstream can read, since every consumer matches a single level. Each
nesting level is judged separately, so an illegal inner factor names itself —
`2 * (w * SUM(x))` reports `w`, not `2`.

The pass does not pre-evaluate a query-wide subquery; composition builds an
expression, so `(SELECT a) * ((SELECT b) * SUM(x))` emits the factor
`(SELECT a) * (SELECT b)` and the values still arrive at execution.

**Composition reassociates, and that is accepted.** `(SUM(x) / 2) / 3` emits
`SUM(x) / (2 * 3)`, which in double arithmetic is not bit-identical to dividing
twice. Decided 2026-08-13: the model carries one numeric domain end to end (§3.6),
the discrepancy is last-ULP (~1e-16 relative, ~35% of random triples), and solver
tolerances are 1e-6 to 1e-9 — ten orders of magnitude coarser. The golden dump
formats with `%.10g`, so a composed scale is not even observable there. The
alternative, a per-factor "is folding safe here?" predicate, is exactly the
per-site pattern §3.6 exists to remove. The known limit is magnitude: composing
factors near the double range can overflow to `inf` or underflow to `0` where
sequential application would not. DeciDB does not special-case this.

### 3.8 Source provenance and tags

Canonical and source form are different concerns. The canonical tree serves
execution; diagnostics must still know what the user wrote. Preserved:

- the original clause identity and spelling;
- whether a value came from an uncorrelated or correlated scalar subquery;
- whether a rebuilt RHS is wholly query-wide or contains row-varying data;
- optimizer mechanism tags (structural, ABS, MIN/MAX, `<>`);
- `WHEN`, `PER` and qualifier ownership.

Provenance is **side-agnostic**: `x <= (SELECT 5)` and `(SELECT 5) >= x` are the
same shared cap after canonicalization and must produce the same actionable
diagnosis. Internal names such as `SUBQUERY` never appear in suggested SQL.

Tags live in the same `alias` field a plain user alias would occupy, so any code
that names an expression for a rejection message must strip them first —
`GetName()` returns the alias whenever one is set, tag or not.
`UserFacingName` (this file) and `ScaleUserName`
(`src/optimizer/decide/decide_linear_form.cpp`) are the two "name it the way the
user wrote it, for an error message" helpers this boundary and stage 5 rely on;
both call `StripDecideTags` before falling back to `ToString()`. This matters on
a re-entry path: a constraint the optimizer rewrote (e.g. an AVG→SUM rewrite
carrying `AVG_REWRITE_TAG`) returns through `AddConstraint` and is
re-canonicalized, so a shape-violation error raised on that second pass could
otherwise quote the internal tag instead of the column the user typed.
`test/common/test_decidb_canonical_verifier.cpp` pins this by tagging a rejected
objective term directly and asserting the `BinderException` names the column,
not the tag.

`FinalizeBoundProvenance` removes any stale root classification, inspects the
complete rebuilt RHS, and stamps `QUERY_WIDE_BOUND_TAG` only when every component
is query-wide.

### 3.9 The objective

An objective is structurally **one side** of a comparison. It has no relation to
orient and no bound to separate, so C2 and C3 have no analogue.
`CanonicalizeObjective` reuses the same `Decompose` / `PeelScale` /
`BuildAdditive` machinery rather than restating it — that sharing is the whole
reason objectives get a boundary instead of their own normalizer.

| ID | Rule | Shared with |
|---|---|---|
| **O0** | A `WHEN`/`PER` wrapper recurses into child 0 only. | C0, same predicate |
| **O1** | The body is an additive spine whose every term references a decision. | — |
| **O2** | A decision-free additive term is folded into `objective_constant_offset`. It shifts the objective without moving `argmax`/`argmin`. One that does not fold to a constant is rejected here. | — |
| **O3** | The spine contains only `+`, binary `-`, unary `-`, and binder casts over decision algebra. Data casts are atoms. | C4, verbatim |
| **O4** | A factor on a decision-bearing reducer is one query-wide value, sits outside the reducer, and has one spelling. | C6, same `PeelScale` |
| **O5** | Structural tags survive rebuilding. The result is a fixed point. | C7 |

Two consequences:

- **`objective_constant_offset` accumulates.** `SetObjective` adds rather than
  assigns, so the constant peeled from what the user wrote survives every later
  rewrite. `VerifyCanonicalObjective` asserts that re-canonicalizing a canonical
  objective peels *nothing*, which is what makes double-counting detectable.
- **A decision-free objective is legal and returned unchanged.**
  `RewriteComposedMinMaxObjectiveTop` deliberately installs a constant placeholder
  and supplies coefficients from `composed_minmax_objective_terms` instead.

Because objectives are canonicalized, `TryMatchScaledAggregate` needs no
`objective_flexible` escape: `AGG * factor` was what an unnormalized clause looked
like, and every consumer now sees `factor * AGG` or `AGG / factor`.

---

## 4. Classification and verification

`ClassifyCanonicalComparison` returns `PER_ROW`, `AGGREGATE` or `INVALID` from
reducer placement plus DECIDE variable scope. Aggregate rows admit only
reducer-rooted decision terms and query-wide top-level terms; scalar decisions are
query-wide, while row/entity decisions and data-varying expressions must be inside
`SUM`/`AVG`/`MIN`/`MAX`. `PER` eligibility is validated against this same
classification after binding, which closed the data-only-reducer loophole the
earlier parsed-shape predicate had. Physical extraction consumes this classifier
rather than re-deciding from the mere presence of an aggregate.

Foldable decision-free bounds that evaluate to NULL are rejected here, with
`COALESCE(...)` guidance, including rebuilt and reversed forms.

`VerifyCanonical` / `VerifyCanonicalObjective` check the observable invariant
without mutating the tree. They perform explicit wrapper, decision-side,
additive-placement and homogeneity checks, then canonicalize a copy and compare
with an ordered, **alias-aware** comparison — required because DuckDB's ordinary
`Expression::Equals` ignores the field carrying DECIDE structural and provenance
tags.

They throw `InternalException`: past this boundary a non-canonical tree is an
engine bug, not user input. Unsupported user syntax has already failed earlier,
through binder/planning validation. Non-comparison leaves stay legal so the
optimizer's `TRUE` placeholders for extracted composed MIN/MAX constraints satisfy
C0.

Verification runs at three points: immediately after user canonicalization, on
each freshly canonicalized subtree entering `AddConstraint` / `SetObjective`, and
on the complete tree at `PhysicalPlanGenerator::CreatePlan(LogicalDecide &)` after
all optimizer rewriting.

`VerifyCanonical` / `VerifyCanonicalObjective` are **debug-only**, the same idiom
as `LogicalOperator::Verify`: the body — including the re-canonicalize-and-compare
fixed-point check — is wrapped in `#ifdef DEBUG`, so it compiles to an empty
function in release. Call sites at all three points above call it unconditionally
regardless of build type. This exists because the check re-canonicalizes the
whole tree a second time purely to assert idempotency, which is too expensive to
pay on every DECIDE query once release is the target; `ValidateCanonicalTree`'s
user-facing `BinderException`s (shape, NULL bound, PER-on-non-aggregate) are a
separate function and are **not** gated — those stay in release. Build `debug` or
`relassert` to exercise the invariant check; `test/common/test_decidb_canonical_verifier.cpp`
pins both the debug-mode throwing behavior and the release-mode no-op via a
`REQUIRE_VERIFY_THROWS` macro that switches on `DEBUG`.

---

## 5. What this stage does not do

- expand products or powers;
- combine like terms;
- fold constants;
- rewrite AVG, ABS, MIN/MAX, `<>`, bilinear products, `norm()` or `IN`;
- look inside reducer bodies;
- evaluate data, reducers, `WHEN`, `PER` or qualifiers;
- choose a solver formulation.

Expression association is fixed by the stage-01 grammar; this stage never
reinterprets a parsed expression tree.

---

## 6. Constraints on future change

- The objective is one side of a comparison, not a second contract. If it seems to
  need a new rule, check first whether the constraint side already has it —
  O0/O3/O4/O5 are the same code as C0/C4/C6/C7, and keeping them shared is the
  point.
- Do not infer cast authorship from a bound cast, and do not add target-type
  exceptions. §3.6 is the settled contract.
- Keep transformation and validation at this boundary. Do not restore a binder,
  optimizer or physical fallback.
- Any new optimizer pass that mutates a constraint in place must go through
  `AddConstraint` rather than editing the tree. C2 is enforced by a throwing check
  at physical extraction, so a rewrite that breaks it fails loudly — but it does
  fail.

---

## 7. Verification

```bash
make release

test/decide/.venv/bin/python3 -m pytest \
  test/decide/tests/test_canonicalize_cast.py \
  test/decide/tests/test_canonicalize_scale.py \
  test/decide/tests/test_canonicalize_sign.py \
  test/decide/tests/test_canonicalize_side_agnostic.py \
  test/decide/tests/test_canonicalize_homogeneity.py \
  test/decide/tests/test_canonicalize_idempotence.py \
  test/decide/tests/test_bilinear.py \
  -v

./test/decide/golden/capture.sh /tmp/canonicalize-after.dump
diff -u test/decide/golden/baseline.dump /tmp/canonicalize-after.dump

make decide-test
git diff --check
```

What each focused file pins:

| File | Contract |
|---|---|
| `test_canonicalize_cast.py` | Mixed BIGINT/DECIMAL against direct DuckDB evaluation around `2^53`; every explicit decision-cast spelling rejected; data casts remain part of bounds and coefficients |
| `test_canonicalize_scale.py` | Composed query-wide multiplication and division around reducers are supported, not rejected |
| `test_canonicalize_side_agnostic.py` | Correlated bounds on both sides |
| `test_canonicalize_homogeneity.py` | Scalar / row / entity scope behavior and the data-only-reducer `PER` case |
| `test_canonicalize_idempotence.py` | Byte-identical emitted models for non-canonical and fixed-point spellings — side swaps, exact casts, scales, `WHEN`, `PER` |
| `test_query_diagnostics_relation.py` | Forward and reversed uncorrelated subquery bounds emit the same editable SQL suggestion |
| `test_bilinear.py` (`TestFactoredProductDegree`) | `SUM((x + y) * z)` is accepted in both clauses |

Passing the suite is necessary but not sufficient. At the start of this work the
tree passed 1,059 tests and all 80 golden models despite an undetected lossy-cast
wrong answer and a reversed-subquery diagnostics defect.

For diagnostics that must continue after a failing DECIDE statement, use a
multi-statement stdin script on one connection — CLI `-c` stops at the first error
and cannot read `decide_diagnostics()` afterward.

---

## 8. Source map

| Concern | Location |
|---|---|
| Canonical transformation (constraints and objective) | `src/planner/decide/decide_canonicalizer.cpp` |
| Contract, in code | `src/include/duckdb/planner/decide/decide_canonicalizer.hpp` |
| Cast policy and unwrapping | `src/decidb/utility/decide_cast_policy.cpp` |
| Cast-policy interface | `src/include/duckdb/decidb/decide_cast_policy.hpp` |
| Parsed cast-authorship validator | `src/planner/expression_binder/decide_binder.cpp` |
| User call site and subquery provenance | `src/planner/binder/query_node/plan_select_node.cpp` |
| Source display capture and rendering | `src/planner/decide/decide_source_provenance.cpp` |
| Generated-constraint / rewritten-objective entry points | `src/planner/operator/logical_decide.cpp` |
| Verification at physical-plan entry | `src/execution/physical_plan/plan_decide.cpp` |
| Canonical model corpus | `test/decide/golden/` |
| Behavior tests | `test/decide/tests/test_canonicalize_*.py` |
