# Stage 02 — Binder: open work

No stage-owned architectural work is open. Two items whose owner is this stage
are tracked elsewhere, and are not restated here:

| Item | Tracked in | Why it lives there |
|---|---|---|
| `DecideDegreeInternal` under-estimates degree through `POWER`, so `SUM(POWER(x,2) * y)` passes the binder gate and is rejected late by physical extraction with a stale, jargon-laden message | [`../../06_issues/code_quality/todo.md`](../../06_issues/code_quality/todo.md) | Opportunistic code-quality finding; the issues tracker is the single home for those |
| Feature gaps that the binder would have to accept — multi-relation reducer qualifiers `sum(D,T: ...)`, row-varying RHS with `PER`, the restricted `decide_when_condition` grammar | [`../../03_expressivity/`](../../03_expressivity/) | They are language-surface decisions, documented per keyword |

If you are picking up the `POWER` fix: the change is ~6 lines in
`src/planner/expression_binder/decide_binder.cpp:100-137` — a `POWER` / `**` case
returning `degree(base) * n` for constant `n`. It needs its own before/after
golden run, because tightening `POWER` can reject shapes that pass today.
