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
2. **Validate** that the result takes a supported shape — per-row, aggregate or value
   (§3.10).

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
composed `MAX(x*v)` and a filtered `sum(when c: x)` are all just decision-bearing
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
| Runtime evaluation | Evaluate coefficients, `when` conditions, `per` classes, `by` groups, a reducer's own key, data reducers, row-varying bounds. Value operations, not shape decisions. |
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
| **C0** | Ordinary `AND` conjunctions recurse into every child. A `when` wrapper, a `per K:` wrapper (`PER_CLASS_TAG`) or the two-level objective's wrapper (`PER_CONSTRAINT_TAG`) recurses into child 0 only; its condition or key columns are copied unchanged. Non-comparison leaves are copied unchanged. The predicates and the walk live in `planner/decide/decide_constraint_walk.hpp` and are shared with every other stage that reads a bound constraint tree, so a new wrapper kind is defined once. |
| **C1** | Each DECIDE comparison has at least one decision-bearing term. |
| **C2** | The right side contains no decision-variable reference. |
| **C3** | Every top-level decision-bearing additive term is on the left; every top-level decision-free additive term is on the right. Data inside a reducer body stays inside that reducer. |
| **C4** | Both additive spines contain only `+`, binary `-`, unary `-`, and binder-generated casts over decision algebra. Data casts are atoms. A factor known before the query runs distributes over an additive child (`2 * (min(x) - 1)` is `2 * min(x) - 2 * 1`), as a negation does; a row-varying factor or a subquery over a sum stays one atom. Like decision terms under such factors merge into one (`x + x` is `2 * x`, `2 * min(x) + min(x)` is `3 * min(x)`); a group whose factors cancel is left as written. The pass never opens an atom or distributes a cast. |
| **C5** | Every comparison takes one shape of §3.10: per-row (no decision reducer), aggregate (decision reducers sharing one `by` key, beside query-wide terms only) or value (any other side with a decision reducer, which the value rewrite turns per-row). A shape §3.10 refuses is rejected here. |
| **C6** | A factor attached to a decision-bearing reducer has one canonical representation, outside the reducer. In the objective it is one value for the whole query; in a constraint it may vary by row, which makes the comparison VALUE. A decision is never a factor. |
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
`SUM(x * price)`, `MAX(x * value)`, `sum(when condition: x)`.

`ReadCanonicalAtoms` reads these atoms back from a canonical side, with each sign
and any reducer factor, by the same spine walk `Decompose` uses. Readers of
canonical trees (the solver path's prepared linear form, direct solve's facts) use
it instead of walking `+` and `-` themselves.

### 3.3a One spelling per meaning: `norm`

The binder spells `norm(e, p)` as a `SUM(e)` aggregate whose alias names the order,
so the aggregate does not mean what it says. This stage replaces orders 1, 2 and
'inf' with their definitions: `SUM(ABS(e))`, `SUM(POWER(e, 2))` and
`MAX(ABS(e))`, keeping the aggregate-local filter and every other tag. The new
aggregate carries `__written_norm_<p>__`, which only rendering reads, so the
canonical text and diagnostics still show `NORM(e, p)`; `RewriteNorm` drops the tag
before the optimizer formulates the clause. This swaps one atom for another and
opens nothing, so the pass stays total. L0 needs indicator variables, which are
formulation, so its marker stays for `RewriteNorm`. `VerifyCanonical` (debug builds)
fails if an L1, L2 or 'inf' marker survives.

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

There is no third placement case for a data-only reducer. It carries no decision, and
the bound side evaluates reducers, so it goes right like any other data: decisions
left, data right, with nothing left floating on both sides.

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

`GetBareDecideColumnRef(expr, decide_index)` sits on top of rule 3: it answers
"is this whole expression one bare decision variable?" for the walkers that need a
coefficient-free term (bilinear operand detection, linear-form factor
classification, the IN marker's target). None strips casts its own way; keeping the
question here keeps rule 3 the only place cast descent is decided.

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

What may be a factor (`PeelScale`, the single place that judges it):

- literals and foldable expressions, uncorrelated scalar subqueries, and arithmetic
  made entirely of query-wide values — anywhere; so too a factor that is identically
  zero before the query runs whatever its data factors hold (`0 * w`, read by
  `PlanTimeSignOf` in `decide_cast_policy`): it is one value, 0, for the whole query, and
  the term it scales vanishes rather than taking the VALUE shape;
- a row-varying column or a correlated subquery — in a constraint, where it is a term
  of each row's constraint: the comparison takes the VALUE shape (§3.10), whose helper
  decision the row's factor multiplies. The objective is produced once, so there it is
  refused: *"DECIDE objective: 'w' varies per row, so it cannot multiply SUM(x). Move
  it inside the aggregate, e.g. SUM(x * w)."*, or, for a correlated subquery, *"this
  subquery returns a different value for each row, ..."*;
- a decision — nowhere: *"DECIDE constraint: 'b' is a decision, so it cannot multiply
  SUM(x). Only data can scale SUM/AVG/MIN/MAX."* (the objective says *"Only constants and
  query-wide values ..."*). A product
  of a decision and a reducer is not a scale.

**Peeling runs to exhaustion.** `2 * (3 * SUM(x))` and `(SUM(x) / 2) / 3` each
yield *one* factor: multipliers compose into `M`, divisors into `D`, and the
emitted factor is `M`, `D`, or `M / D`. Partial peeling would produce a term
nothing downstream can read, since every consumer matches a single level. Each
nesting level is judged separately, so an illegal inner factor names itself —
`2 * (w * SUM(x))` in the objective reports `w`, not `2`.

The pass does not pre-evaluate a query-wide subquery; composition builds an
expression, so `(SELECT a) * ((SELECT b) * SUM(x))` emits the factor
`(SELECT a) * (SELECT b)` and the values still arrive at execution.

**A factor known before the query runs distributes over an additive spine.**
`2 * (min(x) - 1) >= -6` is emitted as `2 * min(x) >= -6 + 2 * 1` and
`(min(x) + 1) / 2 >= -0.5` as `min(x) / 2 >= -0.5 - 1 / 2` (`Decompose` carries the
factor down and `ComposeScale` folds it into each atom's own factor), exactly as a
negation already distributes. Without it the product would be one atom holding a reducer
inside an expression, the VALUE shape, and a bound written that way would never reach the
MIN/MAX rewrite or bound absorption. A factor that varies by row, or a
subquery, is not distributed: the product stays one atom and takes the VALUE shape.

**Like terms merge.** After decomposition, decision atoms that are multiples of one
term, each by a factor known before the query runs (a reducer's peeled factor, none
counting as 1 and a divisor as its reciprocal; for a bare decision the constant factors
it stands under, `PeelBareDecisionFactors`), become one atom scaled by the sum of their
signed factors (`MergeLikeAtoms`): `x + x >= -4` is emitted as `2 * x >= -4`,
`x - 3 * x <= 4` as `-2 * x <= 4`, `2 * min(x) + min(x) >= -6` as `3 * min(x) >= -6`. The
merge runs on each written side and again on the partitioned decision side, where a term
that crossed the relation meets its twin: `3 * x >= x - 4` is `2 * x >= -4`, and the
result is a fixed point (C7). The linear form merged
like terms at extraction anyway, so merging here changes no model row; what it buys is
that a bound written as a repeated term reaches the MIN/MAX rewrite and bound absorption
as the scaled single term it is. A group whose factors cancel (`x - x`) is
left as written rather than dropped, which could empty the side. A row-varying factor
or a subquery is not summable and its atom is not merged.

**A lone negated reducer** carries its sign in its factor:
`-max(e) <= -cap` is emitted as `-1 * max(e) <= -cap`, `-(2 * max(e))` as
`-(2) * max(e)` (`IsLoneNegatedReducer`, `FoldNegationIntoFactor`), and one written with
signs around it, `-(-max(e))`, as the bare term (`IsSignedSpine`). The MIN/MAX rewrite
then reads it as a factor of known sign, the route a `by` can take; left as `-max(e)` it
would be an additive spine, which the composed formulation takes. Whether a factor's sign
is known before the query runs is `ScaleSignAtPlanTime` (`decide_cast_policy`), which
this stage and the MIN/MAX rewrite share.

**Composition reassociates, and that is accepted.** `(SUM(x) / 2) / 3` emits
`SUM(x) / (2 * 3)`, which in double arithmetic is not bit-identical to dividing
twice. The model carries one numeric domain end to end (§3.6),
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
- `when`, `per` and `by` ownership, and a reducer's own key.

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
| **O0** | A `when` wrapper or the two-level objective's wrapper (`PER_CONSTRAINT_TAG`) recurses into child 0 only. | C0, same predicate |
| **O1** | The body is an additive spine whose every term references a decision. | — |
| **O2** | A decision-free additive term is folded into `objective_constant_offset`. It shifts the objective without moving `argmax`/`argmin`. One that does not fold to a constant is rejected here. | — |
| **O3** | The spine contains only `+`, binary `-`, unary `-`, and binder casts over decision algebra. Data casts are atoms. | C4, verbatim |
| **O4** | A factor on a decision-bearing reducer is one query-wide value, sits outside the reducer, and has one spelling (in a constraint it may vary by row, which makes the comparison VALUE, §3.10). | C6, same `PeelScale` |
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
escape for `AGG * factor`: every consumer sees `factor * AGG` or `AGG / factor`.

### 3.10 How a comparison becomes model rows

`ClassifyScoped` reads every canonical comparison's decision side and names its shape
(`ScopedShape`: `ROWS`, `AGGREGATE`, `VALUE`, `UNSUPPORTED`), the rule of
../../language/guide.md §6.4:

| Decision side holds | Shape | Model rows |
|---|---|---|
| no reducer (a data-only reducer may sit on the bound side) | `ROWS` | one per kept row, or per class of an enclosing `per K:` |
| decision reducers sharing one `by` key, under whole-query factors, beside query-wide terms only | `AGGREGATE` | one per `by` group of the kept rows |
| anything else: a per-row or keyed decision beside a reducer, a factor that varies by row, a second `by` key, a reducer inside another expression, a product of decisions beside a reducer with a query-wide decision in the clause (`sum(x) + b * s`) | `VALUE` | the value rewrite (stage 05): each decision reducer becomes a helper decision per `by` group, then one row per class |
| `AVG` under `<>` unless every term is a plain `AVG` | refused | *"DECIDE: AVG in a <> constraint is not supported yet beside another term or with its own when or per"* |

The refusal: an `AVG`'s exclusion under `<>` scales the bound by the group's row count,
exact only when every term is a plain `AVG` over all of them. A query-wide decision
beside a reducer whose body multiplies decisions (`s + sum(b * x)`, `sum(power(x, 2)) +
s`) stays AGGREGATE: the quadratic builder counts it once per grouped row, as the linear
one does (stage 06). A clause
that takes the composed MIN/MAX formulation (`TakesComposedMinMax`: a MIN or MAX over
decisions beside another term, or alone under a factor whose sign is not known before
the query runs) is AGGREGATE only with no `by`, a constant bound and no decision beside
it — that formulation is one row over all of the clause's kept rows under one bound;
otherwise it is VALUE, where each extremum is pinned per group. `PeelScale` lets a
row-varying factor stay on a constraint's reducer as its scale (the comparison is then
VALUE); a factor that is a decision is refused (§3.7). Refusals quote the query through
`QuoteMessagesWith`: columns qualified by relation over a join, a reducer with its
scopes and `by`. `ClassifyCanonicalComparison` maps the shape onto
`CanonicalConstraintClass` (`ROWS` is `PER_ROW`; `UNSUPPORTED`, or a decision on the
bound side, is `INVALID`), so the linear form, the validation in `AddConstraint` and the
debug verifier all read one classification; a VALUE comparison reaching the linear form
is an internal error (the rewrite did not run).

`CanonicalizeTree` stamps an aggregate-shape comparison with its `by` key
(`__group_key_<i>__`) — the one grouping carrier after this stage, since later rewrites
rebuild the reducers. A `per K:` wrapper over it stays, as over every shape
(`IsPerClassWrapper`, a wrapper for every walker): one row per `by` group already stands
for every class, whose constraints differ only in a bound the class determines, so the
wrapper adds no row; it names the classes each group's row stands for, which the linear
form reads as the clause's class key and a diagnosis reports one by one (D28). A
VALUE comparison is stamped `VALUE_REWRITE_TAG`, which the rewrite reads and removes.

---

## 4. Classification and verification

`ClassifyCanonicalComparison` returns `PER_ROW`, `AGGREGATE`, `VALUE` (a comparison
the value rewrite turns into `PER_ROW`, §3.10) or `INVALID`, from `ClassifyScoped`.
Aggregate rows admit only reducer-rooted decision terms and query-wide top-level terms:
a query-wide decision may sit beside the reducers, while a per-row or keyed decision, or
row data, beside them makes the comparison VALUE. Physical extraction consumes this
classifier rather than re-deciding from the mere presence of an aggregate.
`ValidateCanonicalTree` throws `ClassifyScoped`'s refusal for an `INVALID` comparison,
or an `InternalException` when a decision survived on the bound side, which no user
query can cause.

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
user-facing `BinderException`s (the shape refusal, a NULL bound) are a separate
function and are **not** gated — those stay in release. Build `debug` or
`relassert` to exercise the invariant check; `test/common/test_decidb_canonical_verifier.cpp`
pins both the debug-mode throwing behavior and the release-mode no-op via a
`REQUIRE_VERIFY_THROWS` macro that switches on `DEBUG`.

### Scope checks

`BuildConstraintScopeChecks` and `BuildObjectiveScopeChecks` are read-only passes over
the clauses and objective as bound, called once by `Binder::CreatePlan`, after
`PlanSubqueries` (a check's expressions must be copyable) and before `CanonicalizeTree`.
A rewrite's clause entering through
`AddConstraint` builds none. Per clause, in this order: its `when` keeps a row
(`KEEPS_A_ROW`); its `per` key determines each maximal decision- and reducer-free
subexpression, each decision outside reducers and each `by` key (`DETERMINES`); then each
reducer in turn: it reads a row in every `by` group when it has its own `when`
(`READS_A_ROW`), and its own `per` key determines its body within its `by` group
(`DETERMINES`). The objective: its `when`, then `per ()` over its values and decisions,
then each top-level reducer in turn, its `by` key (`DETERMINES`) before its own checks.
The first check to fail stops the query, so this order picks the message. Literals and query-wide values need no check. The message texts are rendered
here, through `RenderDecideSource`, from source fragments and relation aliases
(`DecideScopeChecks`), since only planning still knows them.

---

## 5. What this stage does not do

- expand products or powers;
- combine like terms;
- fold constants;
- rewrite AVG, ABS, MIN/MAX, `<>`, bilinear products, L0 `norm()` or `IN`;
- look inside reducer bodies, except to read them, unchanged, for the scope checks.
  Splitting a body into `coefficient * variable` terms opens terms and can fail, so it
  lives beside this stage in `DecideTermSplitter` (`decide_term_split.cpp`), which reads
  canonical atoms;
- evaluate data, reducers, `when` conditions, `per` classes or `by` groups;
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

./test/decide/golden/check.sh    # diffs baseline.gurobi.dump and baseline.highs.dump

make decide-test
git diff --check
```

What each focused file pins:

| File | Contract |
|---|---|
| `test_canonicalize_cast.py` | Mixed BIGINT/DECIMAL against direct DuckDB evaluation around `2^53`; every explicit decision-cast spelling rejected; data casts remain part of bounds and coefficients |
| `test_canonicalize_scale.py` | Composed query-wide multiplication and division around reducers are supported; a row-varying or correlated factor on a constraint's reducer is read per row; a decision factor is refused |
| `test_canonicalize_side_agnostic.py` | Correlated bounds on both sides |
| `test_canonicalize_homogeneity.py` | A query-wide decision beside a reducer (aggregate shape, under `per` too); a per-row or keyed decision, or a row-varying coefficient on a query-wide one, beside a reducer answered per row |
| `test_canonicalize_idempotence.py` | Byte-identical emitted models for non-canonical and fixed-point spellings — side swaps, exact casts, scales, `when` and `per` wrappers |
| `test_query_diagnostics_relation.py` | Forward and reversed uncorrelated subquery bounds emit the same editable SQL suggestion |
| `test_bilinear.py` (`TestFactoredProductDegree`) | `SUM((x + y) * z)` is accepted in both clauses |

Passing the suite is necessary but not sufficient. At the start of this work the
tree passed 1,059 tests and all 80 golden models despite an undetected lossy-cast
wrong answer and a reversed-subquery diagnostics defect.

To see a diagnosis, prefix the query with `DIAGNOSE` — it returns findings instead of
raising, so a single statement is enough and nothing has to survive an error.

---

## 8. Source map

| Concern | Location |
|---|---|
| Canonical transformation (constraints and objective) | `src/planner/decide/decide_canonicalizer.cpp` |
| Term splitting of canonical atoms | `src/planner/decide/decide_term_split.cpp` |
| Contract, in code | `src/include/duckdb/planner/decide/decide_canonicalizer.hpp` |
| Cast policy and unwrapping | `src/planner/decide/decide_cast_policy.cpp` |
| Cast-policy interface | `src/include/duckdb/planner/decide/decide_cast_policy.hpp` |
| Parsed cast-authorship validator | `src/planner/expression_binder/decide/decide_binder.cpp` |
| User call site and subquery provenance | `src/planner/binder/query_node/plan_select_node.cpp` |
| Source display capture and rendering | `src/planner/decide/decide_source_provenance.cpp` |
| Generated-constraint / rewritten-objective entry points | `src/planner/operator/decide/logical_decide.cpp` |
| Verification at physical-plan entry | `src/execution/physical_plan/plan_decide.cpp` |
| Canonical model corpus | `test/decide/golden/` |
| Behavior tests | `test/decide/tests/test_canonicalize_*.py` |
