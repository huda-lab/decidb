# Prototype design decisions

Status: **ready to begin the first implementation**, with the invariants below
as acceptance criteria. The [experiments](experiments.md) provide the evidence
and its limits. These choices define behavior and ownership; specific DuckDB
classes and optimizer hooks may change when the implementation tests demand it.
No direct rewrite is shipped yet.

## 1. Detection and proof

Attempt the direct path on the complete bound, canonical `LogicalDecide`
*before* `OptimizeDecide` marks it solver-specific. A thin adapter gives rules
exact semantic facts and `unknown` for anything it cannot model. Match may
identify a likely class; Prove must certify every objective term, constraint,
decision scope, runtime value obligation, and output. A miss leaves the
original node untouched. Cost is considered only after proof, and the first
prototype can select its sole proved rule without a cost model.

Why: a flattened solver model is built later, and matching a familiar subtree
can silently drop another term. The first rule's exact admitted shape is in
[the S1 contract](../02_first_rule/spec.md). The adapter must be useful to a
second rule, but only expose facts the first rule actually needs now.

## 2. Result and binding boundary

Build a shared logical result boundary with an **explicit positional mapping**
from each original DECIDE output slot (source columns, then SQL `INTEGER` 0/1
decision) to one generated child output. It advertises the old bindings and
types to parents. It is opaque to transformations that would move an outer
filter or limit into the optimization input, while its child remains ordinary
relational operators. Physical lowering uses the already-resolved child once;
there is no S1-specific physical executor and no global binding substitution.

The first version may retain every mapped output and required score/guard
dependency. That conservative choice passed the bound-plan spike and avoids a
wrong-column bug. Selective output pruning is a later optimization using the
same mapping, with liveness and projection-map tests. The production boundary
should be a core logical operator or an equally owned extension with a narrow,
correct physical-lowering entry point. Exposing the general planner overload
publicly was only a spike expedient. Serializer and parent-context regressions
are required before the boundary is considered implemented.

Why: `LogicalDecide` currently exposes child bindings plus decision bindings.
An ordinary projection allocates new bindings. The first spike's transparent
wrapper let the optimizer prune its child to one `42` column while a parent
still read column 2. Explicit mapping/dependencies fixed the spike.

## 3. Runtime value obligation

For S1, construct one typed score slot per input row, then convert its result
to the solver's DOUBLE domain. A truth-or-error guard checks NULL
and non-finite values before global ranking. Preserve DuckDB errors from
deterministic score expressions that throw. The guard must execute over every
DECIDE input row **when the DECIDE result is consumed**, including unused `x`,
capacity zero, and a parent `LIMIT 1` or filter. An input `WHERE` can remove
rows; an outer `LIMIT 0` can avoid executing the whole DECIDE result.

Initially retain a blocking rank/validation dependency even if the assignment
would be constant or `x` is unused. If optimizer tests show that a relational
guard can be skipped, use a generic blocking validator or narrow admission
until the obligation is proved. A standalone filter is insufficient: it skipped
a late bad row under outer `LIMIT 1` in the experiment. The direct error should
retain the `Invalid Input` category and distinguish NULL from non-finite
coefficients; the solver's exact row-index text is not required. Conversion
errors remain DuckDB conversion errors. No error triggers a solver retry after
execution begins.

## 4. Optimizer movement

The boundary blocks parent transformations that change source rows, cardinality,
or the scope of the global rank. Optimizers may still work *inside* its child
when they preserve the score, validation, and row-to-assignment mapping. The
first version pins required outputs explicitly; it does not claim that generic
projection pruning understands the new boundary. Audit the passes after the
DECIDE optimizer (CTE filter pusher, join order, unused columns, column
lifetime, limit/TopN, late materialization, and later filter rewrites) with
parent filter, join, aggregate, nested, and serializer tests. Any pass that
crosses incorrectly needs a boundary rule in its owning layer.

Why: the spike showed a real post-DECIDE pruning failure, while the corrected
version preserved a parent filter and a 5,000-row late error. Those checks are
evidence for this contract, not proof of every remaining optimizer pass.

## 5. Policy and explanation

Register one DECIDE session setting, `decide_direct_solve`, with validated
`off` (default), `auto`, and `require` values. `off` keeps the solver path;
`auto` uses a proved rewrite or falls back; `require` gives a typed miss reason
instead of solving. `DIAGNOSE` always uses the solver in `off`/`auto` and
conflicts explicitly with `require`. The test-only `DECIDB_FORCE_SOLVER`
override bypasses `auto` and conflicts with `require`; an invalid forced name
under `off`/`auto` must retain the current backend error. Resolve the mode when optimization
builds a plan. A prepared plan keeps that selection until it is rebound or
replanned; changing the setting alone does not silently replace its plan.

One structured record carries mode, selected rule or miss reason, proof facts,
required guards, and whether backend/model work was skipped. Use it for
`require` errors, optimized logical `EXPLAIN`, default physical `EXPLAIN`, and
profiling. The logical boundary can own the record; physical lowering should
attach the same data as generic explain/profile metadata to the ordinary
child. The renderer and profiler must read the metadata explicitly because a
logical-only boundary is absent from physical `EXPLAIN`. The exact metadata
plumbing may change, but visible rule identity cannot be inferred from a
`WINDOW` node alone.

## 6. Numeric contract

Admit an immutable, bound constant capacity whose value is an integer in
`[0, 2^53]`, exactly convertible to DOUBLE and to the window rank's `BIGINT`.
Other bounds remain solver cases in this first slice. Evaluate data-only score
casts with DuckDB semantics before DOUBLE conversion; guard the converted
value. Compare that finite DOUBLE value with zero and rank by it. Keep each
input row's own decision, including duplicate-valued rows. Ties may choose
different rows while preserving the same primary objective.

Do not invent an epsilon that changes whether a positive/negative score is
selected. Both backends sometimes returned zero for tiny positive scores and
disagreed with each other near `1e-9` on a two-row fixture. The mathematical
oracle checks the direct rule; backend differential tests compare feasibility
and primary objective with a measured tolerance, never exact tied vectors.
The [baseline](../03_correctness/baseline.md) records the fixtures and leaves
the final backend tolerance to the permanent test sweep.
