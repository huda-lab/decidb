# Result boundary: keeping DECIDE's output intact

The replacement plan must look exactly like the DECIDE node it replaces: every source column, then one column per
decision, with the same bindings and types. A small **logical-only** operator wraps the generated plan to guarantee
that. It has no problem-specific logic and lowers to ordinary projections, so there is no new physical operator.

## What it does

- **Explicit slot map.** Each original output slot maps to one slot of the generated child. Parents keep reading the
  old bindings.
- **Blocks parent work from crossing it.** A filter or limit above the DECIDE result cannot move into the rank input,
  since that would change which rows are ranked. Optimizing inside the child is fine.
- **Carries the decision record** that `EXPLAIN` and profiling show on a hit (see `policy.md`).

## Why it is explicit

An early spike used a transparent wrapper. The optimizer then pruned the child to one column while a parent still read
column 2, and the query returned wrong data. Declaring one dependency per output, plus the rule's validation slots,
fixed it. Never substitute table indexes globally or re-resolve bindings while lowering.

## Pruning unused columns

An unreferenced output is replaced by a typed NULL placeholder only when skipping it is proved safe: stored columns,
constants, and passthrough aliases through projections, filters, or inner comparison joins. A computed column that could
raise an error stays live, because the solver would have raised it. This is what lets a wide unused payload leave the
rank sort. The validation slots stay live even when no parent reads any output.

## Validation barrier

Runtime checks (NULL or non-finite scores, bad bounds) must run over every input row whenever the DECIDE result is
consumed, even if `x` is unused or the capacity is zero. A standalone filter is not enough; the experiment showed it can
skip a late bad row under `LIMIT 1`. S1's rank window reads every row before it emits one. A rule whose plan could
stream uses `DirectValidationBarrier`: a projected truth-or-error column plus a whole-input window. That holds the rows
in memory, which was chosen over reading the input twice.

## Result types

A declared `BOOL` decision has a 0/1 domain but returns SQL `INTEGER`, as on the solver path.

Details: `direct_result_boundary.cpp` (map checks, serialization, pruning hook) and `direct_builder.cpp` (barrier and
pruning proof). The optimizer passes that run after DECIDE were audited for parent filter, join, aggregate, nested, and
serializer cases; the parent-context rows in `test_direct_three_way.py` and the pruning tests in
`test_direct_user_facing.py` keep those guarded.
