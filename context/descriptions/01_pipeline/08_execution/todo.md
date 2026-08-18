# Stage 08 — Execution: open work

---

## `physical_decide.cpp` is ~4,800 lines and `Finalize` still holds several concerns

**Pointers**: `src/execution/operator/decide/physical_decide.cpp`.

Three of the original four concerns are gone. Term extraction moved to layer 05
(`decide_linear_form.cpp`, 2026-08-15), the data-free emitters moved to layer 06
(`ilp_linearization.cpp`, same day), and the last emitters — the flat and
nested-`PER` MIN/MAX objective, plus the composed MIN/MAX constraint and objective
— followed on 2026-08-18, taking the file from 7,344 to 3,813 lines. Only the
evaluation those composed clauses need stayed behind.

What remains is `Finalize`, still the bulk of the file: three phases plus the ABS
`MAXIMIZE` upper-bound derivation and the auto-`M` refill.

**Decision needed before starting**: whether the remaining split is by phase or by
mechanism. Everything left reads `DecideGlobalSinkState`, so the split has to
decide what that state's interface is rather than just moving code.

**Test**: all 80 golden models byte-identical; `make decide-test` unchanged. This
is a pure refactor, so a non-identical dump is a failure, not a finding.

**Done file**: `done.md` §8 — repoint the source map at the new files.

---

## The `input_column_names` back-fill has two sources and a documented precedence

**Pointers**: `src/execution/physical_plan/plan_decide.cpp:131-165`.

Column names for the unbounded diagnosis come from two places: the child's
`GetColumnBindings()` (a bare source-column reference) and the child projection's
user-written names (harvested *before* `CreatePlan`, because `LogicalProjection`
moves its `expressions` vector away). The rule is that a name referenced in the
clause wins over a raw projection alias, and SELECT-only columns are back-filled
after.

This works, but it is the kind of two-source precedence that silently degrades:
a computed projection expression with no alias produces an empty name, and the
diagnosis then labels an escaping group by position.

**Decision needed**: whether an unnamed computed column should fall back to its
SQL text, to a positional label, or be excluded from `affected_rows` output
entirely. The current behavior is the second by accident rather than by choice.

**Test**: an unbounded query whose `PER` key is a computed expression with no
alias — check what `decide_diagnostics()` calls the escaping group.

**Done file**: `done.md` §7, and
[`../../07_query_diagnostics/unbounded/done.md`](../../07_query_diagnostics/unbounded/done.md).
