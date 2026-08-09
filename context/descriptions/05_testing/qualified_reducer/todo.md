# Relation-Qualified Reducer Test Coverage — Todo

## Aggregate-local WHEN in a constraint — rejected, pinned

`SUM(D: expr) WHEN (cond)` works in an **objective** (covered in `done.md`) and
is **rejected in a constraint**, with a message that leaks the internal
`__qualified_reducer__` tag. Logged in `07_issues/code_quality/todo.md`; pinned
by `test_qualified_reducer_with_aggregate_local_when_in_constraint_rejected`.

Positive coverage for the constraint side belongs with that fix.

## Multi-relation qualifier

`SUM(D,T: ...)` is deferred (`03_expressivity/decide/todo.md`), and its rejection
is pinned. The oracle test for the composite-key semantics belongs with the
implementation.
