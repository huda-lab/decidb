# Shared Harness — completed work

HAR-04: `decide_direct_solve` is a validated session setting with `auto`
(default), `off`, and `require`. A proved hit replaces `LogicalDecide` before
solver selection. An `auto` miss leaves the original node for the solver. A
forced backend or `DIAGNOSE` bypasses `auto` and conflicts with `require`;
invalid forced backend names keep their prior error. Prepared selection is
captured until a real rebind. `require` also fails closed when the DECIDE
optimizer is disabled.

HAR-05: one decision record supplies rule identity, proof facts, and runtime
guards on a hit. A `require` miss raises its reason as an error and builds no
record. Logical and physical `EXPLAIN` and profiling render the record on hits. A miss
under `auto` prints nothing, so the solver plan's `EXPLAIN` is unchanged. The
physical hit is an ordinary projection, with the record carried as metadata.

`DirectProblemFacts` reads the whole language before any solver-specific
rewrite: decision domains and scopes, entity scopes, every constraint with its
comparison or membership, left-side parts (reducer, filter, qualifier, factor,
split terms), right side and its provenance, `PER`/`WHEN` scope, degree and
written clause text, and the objective's parts and scope. Facts own their
expressions. What the adapter cannot model is unknown per constraint or
objective, with a reason. `DirectSolveRule` separates Match, Prove, Cost, Explain,
and Rewrite; the coordinator selects only among proved candidates and maps a
complete relational proposal through one shared result boundary. Its S1 proof
records decision identity/domain, capacity, objective direction/coefficient,
and the all-row guard. The boundary validates slot types and retains the rank
dependency. The 17-case baseline miss matrix and parent/CTE
tests exercise these contracts through SQL.

HAR-01: the read-only adapter models the construct table in
[architecture](../00_design/architecture.md#semantic-facts), splitting bodies with
the same `DecideTermSplitter` the solver path uses. S1 reads only facts:
`s1_rule.cpp` no longer parses bound aggregates, comparisons or functions to
understand the problem. `test_decidb_direct_facts.cpp` checks attribution and
missing provenance, every decision domain and scope, comparisons, membership and
right-side provenance, every reducer kind with filters, qualifiers, factors and
norms, products, squares and nested objective reducers, and an unknown clause
with its reason. Estimates never certify a fact. HAR-02 remains open until a
second rule demonstrates reuse.

The current boundary preserves original bindings and typed output, serializes
dependencies, and passes the focused parent-context tests. Its built-in pass
audit is in [optimizer_audit.md](../00_design/optimizer_audit.md).

HAR-03: the shared boundary checks every original output binding and output
slot type, retains declared validation dependencies, and lowers to one
ordinary physical projection. An unused-column hook replaces only outputs
certified safe to skip with internal typed NULL placeholders. Stored columns,
constants, and passthrough aliases through projections, filters, or inner
comparison joins can be pruned; computed outputs that may throw or have
effects remain live. A miss leaves `LogicalDecide` to the solver;
serializer and parent-query tests cover the accepted path. The optimizer audit
checks that later built-in passes do not move parent filters or limits into the
rank input.

Rule contract (2026-10-03): the rule list lives in `direct_registry.cpp`, and a
connection can replace it with `DirectRuleOverride`, which only tests install. The
coordinator derives the external bindings, decides source-column prunability with
`DirectCanSkipSourceOutput`, and resolves the input's types once; a proposal states
only its output slots, whether each decision output may be skipped, and optional
validation slots. `Cost(proof, context)` sees the estimated source row count, read
without disturbing the plan's cached estimates; the cheapest proved rule wins and a
tie goes to the first registered. `DirectValidationBarrier` builds the all-rows read
for a rule whose plan could stream. Each rule keeps its helpers in a named namespace
(`direct_s1`), because unity builds merge anonymous namespaces across rule files.
`test_decidb_direct_coordinator.cpp` drives the whole path with stub rules: a hit, a
fallback with every rule's reason under `require`, the cheaper rule, the tie, the
estimate `Cost` sees, a non-finite cost and a mismatched output slot as internal
errors, and the barrier raising a late NULL under `LIMIT 1` and `COUNT(*)` while a
streaming check does not.

Shared DECIDE semantics (2026-10-03): `direct_builder.cpp` owns what every rule with
aggregate constraints needs to match the solver. `DirectProjectScope` projects
eligibility (WHEN true and no NULL PER key) with any per-row columns the rule adds;
`DirectGuardEmptyAggregate` raises the empty-aggregate error before any value is
validated; `DirectValidateBounds` checks each data-valued bound for NULL and NaN on
every row, including bypassed ones, reduces it to its group MIN or MAX, refuses an
equality bound that varies within a group, and raises the first failing clause in
source-clause order with the solver's wording (`DirectInvalidBoundMessage`). S1's
ranking, pins and count limits stay in S1. S1's generated plans are unchanged: EXPLAIN
of global, PER/WHEN, aggregate-local WHEN and equality-plus-pin queries is
byte-identical before and after the move.

Shared admission predicates (HAR-06, 2026-10-04): the answers to "is this a value a rule
may use" no longer live in S1. `direct_expression.cpp` has `DirectIsNumericDecisionFree`
(a numeric, deterministic, decision-free coefficient), `DirectSourceNumericColumn` (the
source column a bound is exactly, with its slot, type and the name errors use) and
`DirectIsSourceOnlyNumeric` (a numeric, nonthrowing, deterministic, source-only bound that
`DirectValidateBounds` can check per row). `DirectConstraintFact::PlainSum` answers "is the
left side one plain `SUM`". The bodies moved verbatim from `s1_rule.cpp`; S1's pin
helpers and count normalization stay in S1. The full matrix passes unchanged.
