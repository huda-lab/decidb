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

The first `DirectProblemFacts` adapter reads complete bound objective and
constraint factors, source-clause attribution, decision domains/scopes, and source bindings before any
solver-specific rewrite. Unsupported `PER`/`WHEN` wrappers yield unknown facts
and a reasoned miss. `DirectSolveRule` separates Match, Prove, Cost, Explain,
and Rewrite; the coordinator selects only among proved candidates and maps a
complete relational proposal through one shared result boundary. Its S1 proof
records decision identity/domain, capacity, objective direction/coefficient,
and the all-row guard. The boundary validates slot types and retains the rank
dependency. The 17-case baseline miss matrix and parent/CTE
tests exercise these contracts through SQL.

HAR-01: the read-only adapter reports complete factor membership,
decision scope/domain, source bindings, and exact source-clause attribution for
the first slice. Unsupported scoped wrappers remain ordinary unknown facts.
Two C++ tests use real bound DECIDE plans to check attribution, deliberately
missing provenance, and scoped wrapper status. Estimates never certify a fact.
Broader fact vocabulary for other classes will be added as their proofs need
it. HAR-02 remains open until a second rule demonstrates reuse.

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
