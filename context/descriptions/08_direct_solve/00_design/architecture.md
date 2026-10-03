# Direct Solve Architecture

This document owns the cross-cutting design. [Decisions](decisions.md) states
the first-build contract and [experiments](experiments.md) give its evidence;
[todo.md](todo.md) tracks implementation checks.
The Word catalogue at the directory root is the sole source for problem-class
definitions and proofs.

## The semantic boundary

The direct attempt belongs at the logical DECIDE boundary, before
`DecideOptimizer::OptimizeDecide` marks `LogicalDecide` optimized and before
`ChooseDecideSolver` selects a backend. A rule reads the bound, canonical tree,
not SQL spelling or the late solver-oriented prepared model. The current adapter
is the only direct-solve component that knows today's `LogicalDecide` layout.

```text
current LogicalDecide -> current adapter --\
                                          -> semantic problem -> exact facts -> rules
future ANR -----------> ANR adapter ------/
```

The semantic problem needs decision domains and row/entity/scalar identity;
objective terms and direction; complete constraint factors and comparisons;
reducer `PER`, `WHEN`, qualifier, and later frame semantics; data-valued
expressions; source provenance; and the input plan's bindings. It does not carry
solver choice, Big-M formulation, auxiliary solver variables, or evaluated
coefficient vectors. The adapter models the whole language (see
[Semantic facts](#semantic-facts)) and reports anything else as unknown rather
than silently omitting it. ANR compatibility needs its own adapter contract tests.

## Semantic facts

The adapter's target vocabulary: every construct the bound, canonical tree can hold
when the direct attempt runs, before `OptimizeDecide`. At that point there are no
auxiliary variables, absorbed bounds, or `__minmax_*`, `__ne_*`, `__avg_rewrite__`
and removal-group tags; those belong to the solver path. The canonical form puts
decisions on the left and data on the right, and spells a reducer factor as
`f * AGG` or `AGG / f`.

| Construct | Bound tree | Fact |
|---|---|---|
| `x(BOOL/INT/REAL)` | `decide_variables[i]`, `is_boolean_var`, INTEGER/BIGINT/DOUBLE | domain, output type, index |
| `T.x`, `scalar x` | `variable_scopes`, `entity_scopes`, `entity_key_expressions` | scope; entity scope with relations, key slots, role (declaration or qualifier) |
| `AND`, `WHEN`, `PER` | untagged conjunction; WHEN-tagged `[c, cond]`; PER-tagged `[c, cols]` | ordered constraints; scope with PER keys as source slots and WHEN |
| clause identity | `__source_clause_N__` | source clause id; several facts may share one (`BETWEEN`) |
| `<= < >= > = <>` | canonical `BoundComparisonExpression` | comparison, left-hand parts, right-hand side |
| `x IN (v, ...)` | `BoundOperatorExpression` COMPARE_IN | membership: variable and value expressions |
| per-row or aggregate | reducer placement | `ClassifyCanonicalComparison` |
| right-hand side | constant, source data, `__query_wide_value__`, `__row_varying_subquery__` | data expression with provenance constant, query-wide, or row-varying |
| `SUM`, `AVG`, `MIN`, `MAX` | `BoundAggregateExpression` | reducer kind |
| aggregate-local `WHEN` | `aggregate.filter` | part filter |
| `SUM(D: e)` | alias `__qualified_by_k__` | part qualifier |
| reducer factor | `TryMatchScaledAggregate` | part scale and whether it divides |
| `norm(e, p)` | desugared by the canonicalizer; L0 stays a marker | as its definition; L0 as count-nonzero |
| `x`, `c*x`, `x/c`, `-x` | arithmetic over a decision | linear term: variable and coefficient |
| data term in a reducer | additive data node | constant term |
| `x*y` | product of two decisions | product term |
| `POWER(e, 2)`, `e**2`, `(e)*(e)` | power, `**`, self-product | square of a linear inner expression |
| `ABS(e)` | `abs` | absolute value of a linear inner expression |
| bare `scalar` decision beside a reducer | additive term | row-level part |
| casts | decision casts transparent (`UnwrapDecideCasts`); data casts stay in the coefficient | none |
| objective | sense, `objective_constant_offset`, additive parts, `WHEN`, `PER`, `OUTER(INNER(e)) PER k` | sense, offset, parts, scope; a nested part for `OUTER(INNER(e))` |
| source | child bindings, `source_columns` | bindings and names |

The adapter reads additive atoms with `ReadCanonicalAtoms` and splits each body with
`DecideTermSplitter` (`src/planner/decide/`), the same splitter the solver path's
prepared linear form uses, so the two cannot read one term differently. An unknown
split term is an unknown fact. Degree comes from `DecideExpressionDegree`.
Predicates over data expressions (`DirectMayThrow` and the like) stay in
`direct_expression.cpp`.

## The harness and a rule

| Shared harness | Rule |
|---|---|
| Read-only semantic input and exact fact API | Match a possible class shape |
| Candidate registration and selection | Prove all class-specific eligibility |
| Feature policy and unchanged solver fallback | Construct the class-specific relational assignment |
| Output/identity checks | Declare runtime guards and prove assignment completeness |
| Structured decision record and common test controls | Class-specific positive and near-miss cases |

The rule lifecycle is **Match → Prove → Rewrite → Map → Cost → Explain**. Match
binds semantic parts; Prove produces a typed proof containing every assumption
the builder may use. Unknown facts are a non-fatal miss. Estimates and sampled
data cannot satisfy proof conditions; they belong only in Cost after proof.
Rule order never establishes correctness. A cost model can wait until complete
proofs genuinely compete.

The coordinator keeps the original node untouched during Match and Prove. It
takes its rules from `direct_registry.cpp`, so adding a rule never edits the
coordinator; tests replace the list per connection with `DirectRuleOverride`.
`Cost` receives a `DirectCostContext` holding only the estimated source row count.
The coordinator owns the rule-independent work: it derives the external bindings
(source columns, then one per decision), decides which source columns a parent may
skip, and resolves the input's types once before Rewrite. A
proved candidate constructs a complete proposal: relational child plan, output
mapping, whether each decision output may be skipped, optional validation slots,
and structured explanation. The harness checks that
proposal's schema, types, identity, and guard obligations before committing it.
An expected unsupported shape falls back before commit. An unexpected invariant
or construction failure is an error, not a silently swallowed fallback. After a
direct plan starts, execution errors do not restart via a solver.

## Relational result boundary

Use a small logical-only boundary around the generated ordinary operators.
For each original DECIDE output slot, it owns an explicit mapping to a child
slot, advertises the old binding and type to parents, enforces the scope of
legal optimizer transformations, and carries a structured decision record.
It contains no class algorithm and lowers to its already-resolved ordinary
relational child. The initial build pinned all mapped outputs. The current
unused-column pass prunes an unreferenced output dependency only with a
source-evaluation proof, while always retaining the blocking validation
dependency. The external binding contract does not change.

The boundary blocks parent work that would change the decision input: a filter
above a rank cannot cross below it. Inner query optimization remains
legal if it preserves the score, validation, and row-to-assignment mapping.
The [bound-plan spike](experiments.md) showed that a wrapper advertising old
bindings without child dependencies is unsafe: unused-column removal collapsed
the child to one column. Explicit dependencies kept the plan sound. The
current shared boundary has one dependency per advertised output plus a
mandatory suffix for validation. An unreferenced output becomes an internal
typed NULL placeholder only when skipping its evaluation is proved safe;
otherwise its original dependency stays live. This preserves errors from
computed source columns while allowing unused stored columns to leave a wide
rank through passthrough projections, filters, and inner comparison joins.
Never use a global table-index substitution or rerun binding resolution during
child lowering.

Each admitted plan must preserve input-row cardinality, source-column order and
types, every user decision's SQL type and identity, and surrounding query
semantics. Today a declared `BOOL` decision has a 0/1 domain but returns SQL
`INTEGER`, not SQL `BOOLEAN`.

## First rule in one query

```sql
SELECT id, profit, x
FROM items
DECIDE x(BOOL)
SUCH THAT SUM(x) <= 1
MAXIMIZE SUM(profit * x);
```

For `items = [(1, 9), (2, 10)]`, the optimal complete result is
`[(1, 9, 0), (2, 10, 1)]`. The rule's generated bound operators implement
this relational sketch; it is **not** SQL text to reparse:

```text
items
  -> score = DOUBLE(typed profit expression)
  -> assert score is non-NULL and finite for every consumed input row
  -> rank every row by score DESC using global ROW_NUMBER
  -> x = INTEGER(score > 0 AND rank <= 1)
  -> return every original row with x through the output-slot boundary
```

For minimization, rank ascending and select only negative scores under an
upper-only bound. The current interval rule selects the first `L` free ranks
when a lower bound forces them, then improving scores through `U`, after
subtracting fixed selected rows. Partitioned windows handle proved `PER`
groups. A full-partition count checks a positive lower bound against a
nonempty eligible group. `ORDER BY ... LIMIT 1` alone would
discard the unchosen row, so the window rank feeds a per-row `CASE`
assignment instead. The boundary lets an outer `WHERE id = 1` observe
`(1, 9, 0)` after the global decision is made.

## Runtime validation and outcomes

The rule proof handles structural eligibility. Value-dependent conditions such
as NULL and non-finite coefficients need a runtime check over every S1 input
row when DECIDE executes, even if the parent never projects the decision. A
computed DOUBLE score feeds both an always-true-or-throw guard and the rank.
For scoped aggregates, a separate active-row check precedes score evaluation
so the solver's empty-aggregate error has the same priority. Admitted
source-valued count bounds use blocking all-row NULL/NaN checks and grouped
MIN/MAX reduction between the active check and score evaluation. An equality
bound adds a clause-wide variation count before the ordered validation guard,
preserving error order across different groups. The score guard
sits after the DECIDE input's own filters and before the rank.
Aggregate-local `WHEN` on `SUM(x)` filters the count membership. A
source-valued RHS under that filter reduces over every group row, so its bound
window omits the active partition while ranking keeps it. This distinction is
recorded per source-valued bound.
The rank or an alternative blocking validator must remain live when `x` is
unused or capacity is zero. A standalone filter can skip a late invalid row
under a parent `LIMIT 1`, as the [experiment](experiments.md) showed. An outer
`LIMIT 0` can avoid execution altogether, as on the current solver path.

An admitted direct plan must return a feasible assignment with the same optimal
primary objective, though tied assignments may differ. It must preserve output
schema and all admitted NULL, empty, invalid, infeasible, and unbounded outcomes.
`DIAGNOSE` stays on the solver path until equivalent findings can be returned.
No backend is selected, loaded, or invoked on a committed direct hit.

## Policy and explanation

Use the `decide_direct_solve` session setting with `auto` (default), `off`, and
`require` modes. `auto` attempts a proof then falls back; `require` turns a miss
into a reasoned error. `DIAGNOSE` and a
forced solver bypass `auto` and conflict explicitly with `require`. Resolve
the mode when the plan is built; prepared plans retain their selection until
rebound or replanned. No syntax change is needed.

One structured decision record carries the mode, the selected rule, its proof
facts, and its inserted guards. It exists only for a hit and feeds `EXPLAIN` and
profiling; a `require` miss raises its reason as an error instead. On a
hit the logical boundary owns it and physical lowering passes it to
explain/profile metadata on an ordinary physical operator. A logical-only
node's name alone is insufficient: default physical `EXPLAIN` does not retain
it. A miss under `auto` is silent: the DECIDE node and its `EXPLAIN` are
exactly the solver path's, because with direct solve on by default a miss
record would add internal text to every ordinary DECIDE query.

## Integration and growth

On a miss, the existing path remains `ChooseDecideSolver → solver-specific
rewrites → prepared model → PhysicalDecide`. On a hit, the generated subtree
continues through DuckDB's remaining optimizer passes and existing execution
operators. The DECIDE-specific call site should be small; new rule and harness
code belongs under `src/optimizer/decide/direct/` where possible. Its files are
`direct_problem.cpp` (facts), `direct_coordinator.cpp` (policy, fallback),
`direct_registry.cpp` (the rule list),
`direct_result_boundary.cpp` (output boundary and slot map), `direct_builder.cpp`
(rule-independent plan builders), and one file per rule (`s1_rule.cpp`). A logical
boundary may require small owning-layer changes in planning and serialization.

Build only the shared abstractions exercised by the first rule. A second,
different rule is the reusability test: it must register without rewriting the
coordinator or copying fallback/output-contract logic. Its own mathematical
proof and plan remain its responsibility.
