# Shared Harness — completed work

HAR-04: `decide_direct_solve` is a validated session setting with `off`
(default), `auto`, and `require`. A proved hit replaces `LogicalDecide` before
solver selection. An `auto` miss leaves the original node for the solver. A
forced backend or `DIAGNOSE` bypasses `auto` and conflicts with `require`;
invalid forced backend names keep their prior error. Prepared selection is
captured until a real rebind. `require` also fails closed when the DECIDE
optimizer is disabled.

HAR-05: one decision record supplies rule identity, miss reason, proof facts,
runtime guards, and solver-skipped state. Logical and physical `EXPLAIN` and
profiling render it on hits and misses. The physical hit is an ordinary
projection, with the record carried as metadata.

The first `DirectProblemFacts` adapter reads complete bound objective and
constraint factors, source-clause attribution, decision domains/scopes, and source bindings before any
solver-specific rewrite. Unsupported `PER`/`WHEN` wrappers yield unknown facts
and a reasoned miss. `DirectSolveRule` separates Match, Prove, Cost, Explain,
and Rewrite; the coordinator selects only among proved candidates and maps a
complete relational proposal through one shared result boundary. Its S1 proof
records decision identity/domain, capacity, objective direction/coefficient,
the all-row guard, and external bindings. The boundary validates slot types and
retains the rank dependency. The 17-case baseline miss matrix and parent/CTE
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
slot type, retains mandatory validation dependencies, and lowers to one
ordinary physical projection. An unused-column hook replaces only outputs
certified safe to skip with internal typed NULL placeholders. Stored columns,
constants, and passthrough aliases through projections, filters, or inner
comparison joins can be pruned; computed outputs that may throw or have
effects remain live. A miss leaves `LogicalDecide` to the solver;
serializer and parent-query tests cover the accepted path. The optimizer audit
checks that later built-in passes do not move parent filters or limits into the
rank input.
