# Direct Solve Architecture

This document owns the cross-cutting design. [Decisions](decisions.md) distinguishes
agreed direction from unproved details; [todo.md](todo.md) holds the design gates.
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
coefficient vectors. The first adapter may expose only the facts needed by the
first rule, but must report every unsupported construct as unknown rather than
silently omit it. ANR compatibility needs its own adapter contract tests.

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

The coordinator keeps the original node untouched during Match and Prove. A
proved candidate constructs a complete proposal: relational child plan, output
mapping, required guards, and structured explanation. The harness checks that
proposal's schema, types, identity, and guard obligations before committing it.
An expected unsupported shape falls back before commit. An unexpected invariant
or construction failure is an error, not a silently swallowed fallback. After a
direct plan starts, execution errors do not restart via a solver.

## Relational result boundary

The preferred design is a small logical-only boundary around the generated
ordinary operators. Its job is to expose the removed `LogicalDecide` node's
external column bindings and positional schema, enforce the scope of legal
optimizer transformations, and carry a structured decision record. It must not
contain a class algorithm or become a physical direct-solve executor. Physical
planning should lower it to its ordinary relational child.

This boundary is **not** a blanket optimization fence. An outer filter that
changes the decision input cannot cross a global ranking; harmless work inside
the generated plan and safe removal of unused output columns should remain
possible. The implementation must explicitly account for required score and
guard dependencies under pruning. Current `LogicalDecide` conservatively marks
everything referenced; copying that behavior wholesale would hide performance
cost, not establish the right contract. If a transparent boundary cannot satisfy
DuckDB's binding, serializer, and optimizer rules, use a verified boundary-scoped
remap instead. Never use a global table-index substitution.

Each admitted plan must preserve input-row cardinality, source-column order and
types, every user decision's SQL type and identity, and surrounding query
semantics. Today a declared `BOOL` decision has a 0/1 domain but returns SQL
`INTEGER`, not SQL `BOOLEAN`.

## Runtime validation and outcomes

The rule proof handles structural eligibility. Value-dependent conditions such
as NULL and non-finite coefficients need a mandatory runtime check over the
relevant input rows, even if the parent never projects the decision. A relational
always-true-or-throw filter before ranking is the preferred candidate, not an
assumed guarantee. The guard must be shown to survive pruning and remain after
the DECIDE input's own filters. A generic validation primitive is acceptable
only if existing operators cannot provide the required semantics; it must not
contain S1's assignment algorithm.

An admitted direct plan must return a feasible assignment with the same optimal
primary objective, though tied assignments may differ. It must preserve output
schema and all admitted NULL, empty, invalid, infeasible, and unbounded outcomes.
`DIAGNOSE` stays on the solver path until equivalent findings can be returned.
No backend is selected, loaded, or invoked on a committed direct hit.

## Policy and explanation

Use a DECIDE session setting with `off`, `auto`, and `require` modes. Default is
`off` while evidence is incomplete. `auto` attempts a proof then falls back;
`require` turns a miss into a reasoned test/user error. A forced solver bypasses
`auto` and conflicts explicitly with `require`. Resolve the mode when the plan
is built; prepared-statement behavior must be tested. No syntax change is needed.

One structured decision record should carry mode, selected rule or miss reason,
exact proof facts, inserted guards, and whether solver work was skipped. It is
the source for `EXPLAIN`, profiling, and `require` errors. A logical-only node's
name is not sufficient by itself: default physical `EXPLAIN` may not show it.
The exact rendering mechanism remains a design gate.

## Integration and growth

On a miss, the existing path remains `ChooseDecideSolver → solver-specific
rewrites → prepared model → PhysicalDecide`. On a hit, the generated subtree
continues through DuckDB's remaining optimizer passes and existing execution
operators. The DECIDE-specific call site should be small; new rule and harness
code belongs under `src/optimizer/decide/direct/` where possible. A logical
boundary may require small owning-layer changes in planning and serialization.

Build only the shared abstractions exercised by the first rule. A second,
different rule is the reusability test: it must register without rewriting the
coordinator or copying fallback/output-contract logic. Its own mathematical
proof and plan remain its responsibility.
