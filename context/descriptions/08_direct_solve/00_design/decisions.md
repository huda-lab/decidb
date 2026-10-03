# Prototype design decisions

Status: **first rule implemented and on by default**. These decisions remain the
acceptance criteria. The [experiments](experiments.md) explain why they were
chosen; [correctness/done.md](../03_correctness/done.md) records current test
evidence. The [current built-in optimizer audit](optimizer_audit.md) is complete;
the performance evidence is in the [benefit report](../04_performance/s1_benefit_report.md), and
`auto` is the default (section 7).

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

The later pruning implementation keeps the same external map and required
validation dependency. It prunes only unreferenced outputs whose source
evaluation is proved skippable: stored columns, constants, and passthrough
aliases through projections, filters, or inner comparison joins. Computed
outputs remain live by default because the solver path can
raise an error while evaluating an unused column. A direct/solver regression
uses `error('payload boom')` to enforce this distinction.

Why: `LogicalDecide` currently exposes child bindings plus decision bindings.
An ordinary projection allocates new bindings. The first spike's transparent
wrapper let the optimizer prune its child to one `42` column while a parent
still read column 2. Explicit mapping/dependencies fixed the spike.

## 3. Runtime value obligation

For S1, construct one typed score slot per input row, then convert its result
to the solver's DOUBLE domain. A truth-or-error guard checks NULL
and non-finite values before ranking within the proved scope. Preserve DuckDB errors from
deterministic score expressions that throw. The guard must execute over every
DECIDE input row **when the DECIDE result is consumed**, including unused `x`,
capacity zero, and a parent `LIMIT 1` or filter. An input `WHERE` can remove
rows; an outer `LIMIT 0` can avoid executing the whole DECIDE result. Unrelated
source columns may be NULL and need no guard.

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
or the scope of the rank. Optimizers may still work *inside* its child
when they preserve the score, validation, and row-to-assignment mapping. The
initial version pinned every output explicitly; the current unused-column
pass calls the boundary's opt-in pruning hook. Audit the passes after the
DECIDE optimizer (CTE filter pusher, join order, unused columns, column
lifetime, limit/TopN, late materialization, and later filter rewrites) with
parent filter, join, aggregate, nested, and serializer tests. Any pass that
crosses incorrectly needs a boundary rule in its owning layer.

Why: the spike showed a real post-DECIDE pruning failure, while the corrected
version preserved a parent filter and a 5,000-row late error. Those checks are
evidence for this contract, not proof of every remaining optimizer pass.

## 5. Policy and explanation

Register one DECIDE session setting, `decide_direct_solve`, with validated
`auto` (default), `off`, and `require` values. `auto` uses a proved rewrite or
falls back; `off` keeps the solver path; `require` gives a typed miss reason
instead of solving. `DIAGNOSE` always uses the solver in `off`/`auto` and
conflicts explicitly with `require`. The test-only `DECIDB_FORCE_SOLVER`
override bypasses `auto` and conflicts with `require`; an invalid forced name
under `off`/`auto` must retain the current backend error. Resolve the mode
when optimization builds a plan. A prepared plan keeps that selection until
it is rebound or replanned; changing the setting alone does not silently
replace its plan.

One structured record carries the mode, the selected rule, its proof facts, and
its required guards. It exists only for a hit, and feeds optimized logical `EXPLAIN`,
default physical `EXPLAIN`, and profiling. A `require` miss raises the reason as an
error and builds no record. The logical boundary owns the record, and physical
lowering attaches it as explain/profile metadata to the ordinary child. The
renderer and profiler must read the metadata explicitly because a logical-only
boundary is absent from physical `EXPLAIN`; visible rule identity cannot be
inferred from a `WINDOW` node.

A miss under `auto` is silent. The surviving DECIDE operator and its `EXPLAIN`
are exactly what the solver path prints, with no direct-solve rows, and nothing
is stored on the node. Because direct solve is on by default, a miss record
would otherwise add internal rule text to the `EXPLAIN` of every ordinary
DECIDE query. A user who wants to know why a query was not proved sets
`decide_direct_solve='require'`, which raises the reason.

## 6. Numeric contract

The first build admitted an immutable upper capacity whose value was an
integer in `[0, 2^53]`. The current rule normalizes strict, fractional, and
negative finite consistently foldable numeric bounds to inclusive integer limits. It
admits global bounds and proved source-only `PER`/`WHEN` scopes. Values must
round-trip exactly through DOUBLE and limits must fit within `2^53`;
numeric source-valued bounds can also vary by row. Deterministic, nonthrowing
source-only numeric expressions such as `COALESCE` and `TRY_CAST` are admitted.
Each converts through the
same DOUBLE domain as the solver, validates NULL/NaN before scoring, and
reduces eligible group values. Their limits intersect. Throwing source
expressions remain solver cases because their error order is not yet proved
through the shared window stage.
An aggregate-local `WHEN` on unit `SUM(x)` uses the same count membership as
top-level `WHEN`. For a source-valued RHS, the solver reduces it over all group
rows, including those excluded from the count. Each bound records whether its
MIN/MAX window uses that full group or the top-level `WHEN` membership; the
ranking window always follows the count membership.
Evaluate data-only score
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

## 7. Selection policy

Status: **decided 2026-10-03: `auto` is the default.**

The [benefit report](../04_performance/s1_benefit_report.md) supports it for S1-proved
shapes: direct is 16 to 100 times faster than Gurobi on five-million-row grouped and
global sources with a fraction of the memory, 1.4 to 1.8 times faster on wide full output,
and neutral on small inputs. The only measured loss is about 5% on 100,000 wide rows at
trivial capacity. No completed comparison disagreed on a result.

What a user can notice, by design:

- A tied query may return a different, equally optimal assignment than the solver did.
- Direct errors for NULL/NaN bounds and scores are worded like the solver's. A NULL score
  names its column when the objective is one bare column, as the solver does. For a computed
  score the solver can name whichever columns were NULL on the failing row; the direct plan
  has no row to inspect and uses the solver's generic wording. The solver also reports the
  row number of a non-finite score; the direct plan does not.
- `EXPLAIN` of a query that direct solve proves shows the window plan and its decision
  rows. `EXPLAIN` of any other DECIDE query is unchanged.

Control and escape hatches: `SET decide_direct_solve='off'` always uses the solver;
`'require'` errors with the reason when a query is not proved; `DIAGNOSE` and the test-only
`DECIDB_FORCE_SOLVER` always use the solver (and conflict with `require`).

Consequences already applied to the repository:

- The tests that assert the solver plan (`test_explain.py`) and the solver reference side
  of every differential test pin `decide_direct_solve='off'`, and tests that assert a hit
  use `require`. Nothing relies on the default except `test_direct_solve_is_on_by_default`.
- Goldens, the pipeline profiler, and the forced-backend suites already pin a backend, so
  they never reach direct solve. `benchmark/decide/run_benchmarks.py` tracks the solver
  pipeline's stage timers and pins `off`, because its `p4` query is an S1 shape.
- The direct-solve benchmark runners pin `require` (direct) and `off` (solver) so a
  measurement can never silently use the other path.
- `DECIDB_TEST_DIRECT_SOLVE=off make decide-test` sets the mode for every CLI call in the
  suite, as `DECIDB_FORCE_SOLVER` pins a backend. With the default, an S1-shaped query in the
  wider suite runs through direct solve and no longer reaches the solver, so both runs
  are needed. Under `require` for every call, 614 of the 1,860 tests pass (an upper bound on
  how many reach direct solve, since non-DECIDE tests pass too); under `off` and under the
  default all pass.

Cost-based choice between competing direct plans is a separate question (NEXT-05).
